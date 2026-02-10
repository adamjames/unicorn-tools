/*
 * Cosmic Stream Launcher
 * Uses Luma3DS custom SVC to launch sysmodule
 */

#include <3ds.h>
#include <stdio.h>
#include <string.h>

// NIM title ID - our sysmodule replaces this
#define COSMIC_STREAM_TID 0x0004013000002C02ULL

// Luma3DS custom SVC for service control
extern Result svcControlService(u32 op, void* out, const char* name);

// PM launch wrapper
static Result pmLaunchTitle(Handle pmHandle, u64 titleId, u32 flags)
{
    u32 *cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = IPC_MakeHeader(0x1, 5, 0);
    cmdbuf[1] = titleId & 0xFFFFFFFF;
    cmdbuf[2] = (titleId >> 32) & 0xFFFFFFFF;
    cmdbuf[3] = MEDIATYPE_NAND;
    cmdbuf[4] = 0; // update_type
    cmdbuf[5] = flags;

    Result ret = svcSendSyncRequest(pmHandle);
    if (R_SUCCEEDED(ret)) ret = cmdbuf[1];
    return ret;
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    printf("Cosmic Stream Launcher\n");
    printf("======================\n\n");

    Handle pmHandle = 0;
    Result rc;

    printf("Getting pm:app handle...\n");

    // Use Luma3DS SVC to steal pm:app session
    rc = svcControlService(0, &pmHandle, "pm:app");
    if (R_FAILED(rc)) {
        printf("svcControlService: %08lX\n\n", rc);
        printf("This requires Luma3DS with\n");
        printf("custom SVCs enabled.\n\n");
        printf("Alternative: run\n");
        printf("cosmic_stream.3dsx directly.\n");
        goto wait_exit;
    }
    printf("Got pm:app: OK\n\n");

    printf("Launching sysmodule...\n");
    printf("TID: %016llX\n\n", COSMIC_STREAM_TID);

    rc = pmLaunchTitle(pmHandle, COSMIC_STREAM_TID, 1);
    if (R_FAILED(rc)) {
        printf("Launch failed: %08lX\n\n", rc);
        printf("Ensure CXI is in:\n");
        printf("/luma/sysmodules/\n\n");
        printf("And enable external modules\n");
        printf("in Luma config (SELECT@boot)\n");
    } else {
        printf("SUCCESS!\n\n");
        printf("Cosmic Stream is now\n");
        printf("running in background.\n");
    }

    svcCloseHandle(pmHandle);

wait_exit:
    printf("\nPress START to exit.\n");

    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    gfxExit();
    return 0;
}
