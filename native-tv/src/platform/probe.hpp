#pragma once

namespace nativetv {

// Exit codes of `nativetv-tv --probe`. The install gates the Home takeover on
// zero, so each failure class has its own code to tell an owner why the
// native Home was refused. 1 is left for the client's own errors.
enum ProbeExit {
    kProbeOk = 0,
    kProbeNoDisplay = 10,  // SDL's video system did not start
    kProbeNoContext = 11,  // no window, or no GLES 2 context on it
    kProbeGlError = 12,    // shader build, a GL call or a texture limit below what the client needs failed
    kProbeReadback = 13,   // the frame drew, but the pixel read back is not the colour drawn
};

// Brings up the same window and context as the client, draws one frame with a
// shader, reads a pixel back and prints one JSON line on stdout:
//   {"ok":true,"renderer":"...","version":"...","maxTextureSize":8192,
//    "highpFragment":true,"width":1920,"height":1080,"pixel":[51,153,204,255]}
// or {"ok":false,"stage":"display|context|gl|readback","error":"..."}.
// The environment variable NATIVETV_PROBE_FAULT=gl-error|readback injects the
// last two failures so the exit codes can be tested on a healthy machine.
int runProbe(bool windowed);

} // namespace nativetv
