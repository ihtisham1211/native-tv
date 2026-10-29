#include "service/app_package_inspector.hpp"
#include "core/sha256.hpp"

#include <zlib.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace nativetv;
using namespace nativetv::service;

namespace {
using Bytes = std::vector<unsigned char>;

void append(Bytes &target, const Bytes &source) {
    target.insert(target.end(), source.begin(), source.end());
}

void put(Bytes &target, size_t offset, const std::string &text) {
    assert(offset + text.size() <= target.size());
    std::memcpy(target.data() + offset, text.data(), text.size());
}

std::string octal(uint64_t value, size_t width) {
    std::string text;
    do {
        text.insert(text.begin(), static_cast<char>('0' + value % 8));
        value /= 8;
    } while (value);
    while (text.size() < width)
        text.insert(text.begin(), '0');
    return text;
}

Bytes tarEntry(const std::string &name, const Bytes &body, char type = '0',
               const std::string &link = std::string()) {
    Bytes header(512, 0);
    put(header, 0, name.substr(0, 100));
    put(header, 100, "0000644");
    put(header, 108, "0000000");
    put(header, 116, "0000000");
    put(header, 124, octal(body.size(), 11));
    put(header, 136, "00000000000");
    put(header, 148, "        ");
    header[156] = static_cast<unsigned char>(type);
    if (!link.empty()) put(header, 157, link.substr(0, 100));
    put(header, 257, "ustar");
    put(header, 263, "00");
    unsigned checksum = 0;
    for (unsigned char value : header) checksum += value;
    put(header, 148, octal(checksum, 6));
    header[154] = 0;
    header[155] = ' ';
    Bytes result = header;
    append(result, body);
    result.resize(result.size() + (512 - body.size() % 512) % 512, 0);
    return result;
}

Bytes tarArchive(const std::vector<Bytes> &entries) {
    Bytes result;
    for (const Bytes &entry : entries) append(result, entry);
    result.resize(result.size() + 1024, 0);
    return result;
}

Bytes gzip(const Bytes &input) {
    z_stream stream = {};
    assert(deflateInit2(&stream, Z_BEST_SPEED, Z_DEFLATED, MAX_WBITS + 16,
                        MAX_MEM_LEVEL, Z_DEFAULT_STRATEGY) == Z_OK);
    Bytes output(compressBound(input.size()) + 64);
    stream.next_in = const_cast<Bytef *>(input.data());
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = output.data();
    stream.avail_out = static_cast<uInt>(output.size());
    assert(deflate(&stream, Z_FINISH) == Z_STREAM_END);
    output.resize(stream.total_out);
    deflateEnd(&stream);
    return output;
}

Bytes elf(unsigned machine) {
    Bytes bytes(64, 0);
    bytes[0] = 0x7f;
    bytes[1] = 'E';
    bytes[2] = 'L';
    bytes[3] = 'F';
    bytes[4] = 1;
    bytes[5] = 1;
    bytes[6] = 1;
    bytes[18] = static_cast<unsigned char>(machine & 0xff);
    bytes[19] = static_cast<unsigned char>((machine >> 8) & 0xff);
    return bytes;
}

Bytes arMember(const std::string &name, const Bytes &body) {
    Bytes header(60, ' ');
    put(header, 0, name + "/");
    put(header, 16, "0");
    put(header, 28, "0");
    put(header, 34, "0");
    put(header, 40, "100644");
    put(header, 48, std::to_string(body.size()));
    header[58] = '`';
    header[59] = '\n';
    Bytes result = header;
    append(result, body);
    if (body.size() % 2) result.push_back('\n');
    return result;
}

Bytes validPackage(const std::vector<Bytes> &extraData = {}) {
    const std::string appId = "com.example.app";
    std::vector<Bytes> controlEntries;
    const std::string controlText =
        "Package: com.example.app\nVersion: 1.2.3\nArchitecture: all\n";
    controlEntries.push_back(tarEntry("./control", Bytes(controlText.begin(), controlText.end())));
    Bytes control = gzip(tarArchive(controlEntries));

    std::vector<Bytes> dataEntries;
    dataEntries.push_back(tarEntry("./usr/", Bytes(), '5'));
    dataEntries.push_back(tarEntry("./usr/palm/applications/" + appId + "/", Bytes(), '5'));
    const std::string appInfo = "{\"id\":\"com.example.app\",\"title\":\"Example\","
                                "\"version\":\"1.2.3\",\"icon\":\"icon.png\"}";
    dataEntries.push_back(tarEntry("./usr/palm/applications/" + appId + "/appinfo.json",
                                   Bytes(appInfo.begin(), appInfo.end())));
    dataEntries.push_back(tarEntry("./usr/palm/applications/" + appId + "/bin/run",
                                   elf(40)));
    dataEntries.push_back(tarEntry("./usr/palm/services/com.example.app.service/", Bytes(), '5'));
    dataEntries.push_back(tarEntry("./usr/palm/services/com.example.app.service/run", elf(40)));
    for (const Bytes &entry : extraData) dataEntries.push_back(entry);
    Bytes data = gzip(tarArchive(dataEntries));

    Bytes package;
    const std::string magic = "!<arch>\n";
    package.insert(package.end(), magic.begin(), magic.end());
    append(package, arMember("debian-binary", Bytes({'2', '.', '0', '\n'})));
    append(package, arMember("control.tar.gz", control));
    append(package, arMember("data.tar.gz", data));
    return package;
}

void writeFile(const std::string &path, const Bytes &bytes) {
    FILE *file = std::fopen(path.c_str(), "wb");
    assert(file);
    assert(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
    assert(std::fclose(file) == 0);
}

std::string sha256(const Bytes &bytes) {
    crypto::Sha256 hash;
    hash.update(bytes.data(), bytes.size());
    return crypto::hexDigest(hash.finish());
}

} // namespace

int main() {
    char directoryTemplate[] = "/tmp/nativetv-app-inspector-XXXXXX";
    char *directoryValue = mkdtemp(directoryTemplate);
    assert(directoryValue);
    const std::string directory(directoryValue);
    const std::string validPath = directory + "/valid.ipk";
    const std::string unsafePath = directory + "/unsafe.ipk";
    const std::string linkPath = directory + "/link.ipk";
    const Bytes valid = validPackage();
    writeFile(validPath, valid);

    Json result;
    std::string error;
    assert(AppPackageInspector::inspect(directory, "valid.ipk", result, error));
    assert(result["package"].string() == "com.example.app");
    assert(result["version"].string() == "1.2.3");
    assert(result["sha256"].string() == sha256(valid));
    assert(result["apps"].size() == 1);
    assert(result["apps"][0]["id"].string() == "com.example.app");
    assert(result["apps"][0]["title"].string() == "Example");
    assert(result["apps"][0]["icon"].string() == "icon.png");
    assert(result["services"].size() == 1);
    assert(result["services"][0].string() == "com.example.app.service");
    assert(result["machines"].size() == 1);
    assert(result["machines"][0].string() == "arm");

    assert(!AppPackageInspector::inspect(directory, "../valid.ipk", result, error));
    assert(error == "the file is not in the staging directory");

    const Bytes unsafe = validPackage({tarEntry("../../etc/passwd", Bytes({'x'}))});
    writeFile(unsafePath, unsafe);
    assert(!AppPackageInspector::inspect(directory, "unsafe.ipk", result, error));
    assert(error.find("unsafe path") != std::string::npos);

    const Bytes escapingLink = validPackage({tarEntry(
        "./usr/palm/applications/com.example.app/sub/link", Bytes(), '2',
        "../../../../../../etc/passwd")});
    writeFile(linkPath, escapingLink);
    assert(!AppPackageInspector::inspect(directory, "link.ipk", result, error));
    assert(error.find("unsafe path") != std::string::npos);

    const std::string symlinkPath = directory + "/symlink.ipk";
    assert(symlink(validPath.c_str(), symlinkPath.c_str()) == 0);
    assert(!AppPackageInspector::inspect(directory, "symlink.ipk", result, error));
    unlink(symlinkPath.c_str());
    unlink(linkPath.c_str());
    unlink(unsafePath.c_str());
    unlink(validPath.c_str());
    rmdir(directory.c_str());
    std::cout << "native app package inspector assertions passed\n";
    return 0;
}
