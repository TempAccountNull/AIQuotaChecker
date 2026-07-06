# AIQuotaChecker project layout

Root intentionally contains only:

- `AIQuotaChecker.sln`
- `AIQuotaChecker.vcxproj`
- `src/`

Source layout:

```text
src/Project              main Win32/ImGui entry point
src/Resources            rc, icons, Resource.h, framework.h, targetver.h
src/Settings             app settings persistence/types
src/Clock                shared clock helpers
src/Format               formatting helpers
src/Json                 JSON helper singleton
src/Math                 math helper singleton
src/Network              WinHTTP/network helper singleton
src/Text                 text helper singleton
src/Renderer             ImGui renderer
src/ResetTime            reset countdown display modes
src/Win32                native notification window
src/Window               usage window model helpers
src/Providers/Codex      Codex provider, snapshot parser, notifier
src/Providers/Claude     Claude provider, snapshot parser, notifier
src/Providers/ZAi        Z.Ai provider, snapshot parser, notifier
src/Tools                helper scripts
```

- ResetTime: reset countdown modes, including static, digital, and per-digit flip-clock rendering support.
- Flip mode renders each HH:MM:SS digit as its own split box and flips only the digits that change.


## Global

```text
src/Global/Global.hpp  - stable precompiled include header
src/Global/Global.cpp  - creates the Visual Studio PCH
```
