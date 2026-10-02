# Third-party components

- SQLite 3.53.4, public domain. Statically linked for the local character upload queue. Source: https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip . License: https://www.sqlite.org/copyright.html .

- miniz 3.0.2, Copyright (c) Rich Geldreich, Tenacious Software LLC, RAD Game Tools and Valve Software, MIT license. Only its inflater is linked. https://github.com/richgel999/miniz/tree/3.0.2
- JSON for Modern C++ 3.11.3, Copyright (c) Niels Lohmann, MIT license. https://github.com/nlohmann/json/tree/v3.11.3

- Dear ImGui v1.91.9b, Copyright (c) 2014-2025 Omar Cornut, MIT License. https://github.com/ocornut/imgui
- WinDivert 2.2.2, Copyright (c) basil, LGPL v3 or GPL v2 (upstream dual license). This project dynamically links the unmodified DLL under the LGPL option. Source and corresponding release: https://github.com/basil00/WinDivert/tree/v2.2.2 . The distribution includes the original signed driver. The DLL remains replaceable.

Original license texts are installed in `licenses/`. Windows SDK components are supplied by Microsoft. The system Microsoft YaHei font is loaded from Windows and is not redistributed.
