#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

// Test-only host. Its caller supplies a verified copy of SupportCaptureFixture,
// never a real UEVR backend; keep the process alive for read-only bundle tests.
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) { return 2; }
    const auto fixture = LoadLibraryW(argv[1]);
    if (!fixture) { return 3; }
    Sleep(30000);
    FreeLibrary(fixture);
    return 0;
}
