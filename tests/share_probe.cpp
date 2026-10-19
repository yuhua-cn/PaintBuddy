// 验证：CRT 以「追加写」持有当天 jsonl 时，分析线程能否读到它。
// v1 已证明：fopen(...,"ab") 持有句柄时，读取方无论请求哪种共享模式都失败(err=32)。
// 本 v2 验证修复方向：写入端改用 _fsopen(..., _SH_DENYNO) 后，
// 读取端请求 FILE_SHARE_READ|FILE_SHARE_WRITE 即可并发读取。
#include <windows.h>
#include <cstdio>
#include <share.h>

static int TryRead(DWORD share, long long& outSize) {
    HANDLE h = CreateFileW(L"pb_share_probe.tmp", GENERIC_READ, share, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return (int)GetLastError();
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    outSize = sz.QuadPart;
    CloseHandle(h);
    return 0;
}

int main() {
    const char* path = "pb_share_probe.tmp";
    int failures = 0;

    // —— 场景 A：现状 fopen(...,"ab") ——
    {
        FILE* f = nullptr;
        if (fopen_s(&f, path, "ab") != 0 || !f) { printf("fopen failed\n"); return 1; }
        fwrite("hello\n", 1, 6, f); fflush(f);
        long long sz = 0;
        int e1 = TryRead(FILE_SHARE_READ, sz);
        int e2 = TryRead(FILE_SHARE_READ | FILE_SHARE_WRITE, sz);
        printf("[A] fopen(ab)      reader SHARE_READ=%d  SHARE_READ|WRITE=%d\n", e1, e2);
        if (e1 != 0 || e2 != 0) ++failures;   // 预期：现状读不到
        fclose(f);
        DeleteFileW(L"pb_share_probe.tmp");
    }

    // —— 场景 B：修复后 _fsopen(...,_SH_DENYNO) ——
    {
        FILE* f = _fsopen(path, "ab", _SH_DENYNO);
        if (!f) { printf("_fsopen failed err=%d\n", errno); return 1; }
        fwrite("hello\n", 1, 6, f); fflush(f);
        long long sz = 0;
        int e2 = TryRead(FILE_SHARE_READ | FILE_SHARE_WRITE, sz);
        printf("[B] _fsopen(DENYNO) reader SHARE_READ|WRITE=%d size=%lld\n", e2, sz);
        // 写入端仍应能继续追加
        fwrite("world\n", 1, 6, f); fflush(f);
        long long sz2 = 0;
        int e3 = TryRead(FILE_SHARE_READ | FILE_SHARE_WRITE, sz2);
        printf("[B] after 2nd write   reader=%d size=%lld (expect 12)\n", e3, sz2);
        if (e2 != 0 || sz != 6 || e3 != 0 || sz2 != 12) ++failures;
        fclose(f);
        DeleteFileW(L"pb_share_probe.tmp");
    }

    printf(failures == 0
        ? "RESULT: CONFIRMED - writer must use _SH_DENYNO and reader must allow WRITE share\n"
        : "RESULT: UNEXPECTED (see above)\n");
    return failures == 0 ? 0 : 1;
}
