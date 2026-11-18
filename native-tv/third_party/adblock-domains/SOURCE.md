# Advertising host data

The common advertising and measurement host entries in
`src/service/adblock.cpp` are reproduced from `legacy lib/adblock-domains.js`.
That list was reviewed against StevenBlack/hosts at commit
`0ced9c8a871b668a31fe194f2801d80ee046bf31` on 2026-10-18.

The host data is distributed under the MIT License:

Copyright (c) 2026 Steven Black

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## LG and partner host names

The LG, Alphonso, Pluto, Xumo and Google push names in `src/service/adblock.cpp`
that the list above lacks were taken from the lists of
[Glasshouse](https://github.com/rorygallagher2024/glasshouse) 0.156.0
(`server/lib/privacy.js` at commit `e8d01d494e64059fc708f46ca423998930b9c24b`,
17 November 2026, MIT, Copyright (c) 2026 Rory Gallagher), with the tier each
one is in. Glasshouse says it classified the SmartAd, screen recognition,
performance reporting, Home, ThinQ, SDP and LG Channels names against
[Life's Good Without Spying](https://github.com/lewisdoesstuff/lifesgoodwithoutspying)'s
domain lists at commit `0057ce44d5b4494ad85cf5975d599c58d5f25871` (MIT,
Copyright (c) 2026 Lewis Wood), and that notice is kept beside this one in
[LICENSE-lifesgoodwithoutspying.txt](LICENSE-lifesgoodwithoutspying.txt). Native Tv
did not review that project's lists itself, and none of these names was seen in
a capture of ours, apart from those `docs/development/tv-research.md` records
from a CX and a C5. What Glasshouse measured on its own TVs is marked as such
there and in the comments of `adblock.cpp`.

Names that LG puts a region or the TV's country in front of are listed bare
and for LG's regions and the TV's country only, as Glasshouse does, rather than
for every country that list names.
