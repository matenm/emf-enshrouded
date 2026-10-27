#pragma once
// The text that marks EMF's loader DLL (exported as emf_loader_identity, so the linker keeps it). The installer, the
// uninstaller and the core recognise EMF's loader by it: plain words such as "emf_loader" also appear in tools that
// merely mention EMF's files. package/emf_common.ps1 has the same text.
#define EMF_LOADER_MARKER "EMF-LOADER-7d3c9f"
