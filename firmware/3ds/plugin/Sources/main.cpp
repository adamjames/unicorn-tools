#include <3ds.h>
#include <CTRPluginFramework.hpp>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <malloc.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

// Streaming configuration
static char cfg_host[64] = "mitre.lan";
static int cfg_port = 8080;
static int cfg_fps = 20;
static bool streaming_enabled = true;
static volatile bool thread_running = false;
static u32 *socBuffer = nullptr;
static CTRPluginFramework::Task *stream_task = nullptr;

#define FRAME_WIDTH 32
#define FRAME_HEIGHT 32
#define FRAME_SIZE (FRAME_WIDTH * FRAME_HEIGHT * 3)

namespace CTRPluginFramework
{
    // Log to SD card for debugging
    static void Log(const std::string &msg)
    {
        File file;
        if (File::Open(file, "/cosmic_debug.log", File::WRITE | File::APPEND | File::CREATE) == 0)
        {
            file.Write(msg.c_str(), msg.length());
            file.Write("\n", 1);
            file.Close();
        }
    }

    // Read config from SD card
    static void LoadConfig(void)
    {
        Log("LoadConfig() called");
        File file;
        if (File::Open(file, "/3ds/cosmic_stream.cfg", File::READ) == 0)
        {
            Log("Config file opened");
            char buf[256];
            int bytesRead = file.Read(buf, sizeof(buf) - 1);
            if (bytesRead > 0)
            {
                buf[bytesRead] = '\0';
                Log("Config read: " + std::to_string(bytesRead) + " bytes");
                char *line = strtok(buf, "\n");
                while (line)
                {
                    while (*line == ' ') line++;
                    if (*line != '#' && *line != '\0')
                    {
                        char *eq = strchr(line, '=');
                        if (eq)
                        {
                            *eq = '\0';
                            char *val = eq + 1;
                            if (strcmp(line, "host") == 0)
                            {
                                strncpy(cfg_host, val, sizeof(cfg_host) - 1);
                                Log("host=" + std::string(cfg_host));
                            }
                            else if (strcmp(line, "port") == 0)
                            {
                                cfg_port = atoi(val);
                                Log("port=" + std::to_string(cfg_port));
                            }
                            else if (strcmp(line, "fps") == 0)
                            {
                                cfg_fps = atoi(val);
                                Log("fps=" + std::to_string(cfg_fps));
                            }
                        }
                    }
                    line = strtok(nullptr, "\n");
                }
            }
            file.Close();
        }
        else
        {
            Log("Config file not found, using defaults");
        }
        Log("Final config: host=" + std::string(cfg_host) + " port=" + std::to_string(cfg_port) + " fps=" + std::to_string(cfg_fps));
    }

    // Downsample framebuffer to 32x32
    // Top screen is 400x240, rotated 90 degrees CCW in memory (BGR format)
    static void DownsampleFrame(const u8 *src, int srcW, int srcH, u8 *dst)
    {
        int stepX = srcW / FRAME_WIDTH;
        int stepY = srcH / FRAME_HEIGHT;

        for (int y = 0; y < FRAME_HEIGHT; y++)
        {
            for (int x = 0; x < FRAME_WIDTH; x++)
            {
                int srcX = x * stepX;
                int srcY = y * stepY;
                // 3DS framebuffer is rotated 90 CCW, BGR format
                // Physical layout: column-major, bottom-to-top within column
                int srcIdx = (srcX * srcH + (srcH - 1 - srcY)) * 3;
                int dstIdx = (y * FRAME_WIDTH + x) * 3;
                dst[dstIdx + 0] = src[srcIdx + 2]; // R
                dst[dstIdx + 1] = src[srcIdx + 1]; // G
                dst[dstIdx + 2] = src[srcIdx + 0]; // B
            }
        }
    }

    // Cached connection state (initialized once, reused for all frames)
    static int udp_sock = -1;
    static struct sockaddr_in target_addr;
    static bool connection_ready = false;

    // Initialize UDP socket and resolve host (call once from main context)
    static bool InitConnection(void)
    {
        Log("InitConnection: resolving " + std::string(cfg_host));

        struct hostent *host = gethostbyname(cfg_host);
        if (!host)
        {
            Log("DNS resolution failed");
            return false;
        }

        memset(&target_addr, 0, sizeof(target_addr));
        target_addr.sin_family = AF_INET;
        target_addr.sin_port = htons(cfg_port);
        memcpy(&target_addr.sin_addr, host->h_addr, host->h_length);

        Log("Resolved to " + std::string(inet_ntoa(target_addr.sin_addr)));

        udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (udp_sock < 0)
        {
            Log("UDP socket creation failed");
            return false;
        }

        Log("UDP socket created");
        connection_ready = true;
        return true;
    }

    static void CloseConnection(void)
    {
        if (udp_sock >= 0)
        {
            closesocket(udp_sock);
            udp_sock = -1;
        }
        connection_ready = false;
    }

    // Send frame via UDP - fast, no connection overhead
    static int SendFrame(const u8 *frame)
    {
        if (!connection_ready || udp_sock < 0)
            return -1;

        int sent = sendto(udp_sock, (const char*)frame, FRAME_SIZE, 0,
                          (struct sockaddr*)&target_addr, sizeof(target_addr));

        if (sent != FRAME_SIZE)
            return -2;

        return 0;
    }

    // Background streaming task (using CTRPluginFramework::Task)
    // NOTE: socInit and InitConnection must be called from main context before this task starts
    static s32 StreamTaskFunc(void *arg)
    {
        Log("StreamTaskFunc started");
        u8 frame[FRAME_SIZE];
        u64 delay = 1000000000ULL / cfg_fps;

        OSD::Notify("Stream: started to " + std::string(cfg_host));
        Log("Streaming to " + std::string(cfg_host) + ":" + std::to_string(cfg_port));

        int frameCount = 0;
        int errorCount = 0;

        while (thread_running && streaming_enabled)
        {
            // Use CTRPluginFramework's Screen class to get framebuffer
            // This is safer and works within the plugin context
            const Screen &topScreen = OSD::GetTopScreen();

            // Get raw framebuffer pointer - top screen is 400x240
            u8 *fb = (u8*)topScreen.LeftFramebuffer;
            if (fb)
            {
                DownsampleFrame(fb, 400, 240, frame);
                int result = SendFrame(frame);
                if (result == 0)
                {
                    frameCount++;
                    if (frameCount == 1)
                    {
                        Log("First frame sent successfully");
                    }
                    if (frameCount == 1 || frameCount % 100 == 0)
                    {
                        OSD::Notify("Stream: sent " + std::to_string(frameCount) + " frames");
                        Log("Sent " + std::to_string(frameCount) + " frames");
                    }
                    errorCount = 0;
                }
                else
                {
                    errorCount++;
                    if (errorCount == 1)
                    {
                        Log("First send error: " + std::to_string(result));
                    }
                    if (errorCount == 1 || errorCount % 50 == 0)
                    {
                        // -1=socket, -2=DNS, -3=connect, -4=send hdr, -5=send data
                        OSD::Notify("Stream: err " + std::to_string(result) + " #" + std::to_string(errorCount), Color::Orange);
                        Log("Send error: " + std::to_string(result) + " count=" + std::to_string(errorCount));
                    }
                }
            }
            else if (frameCount == 0)
            {
                OSD::Notify("Stream: no framebuffer!", Color::Red);
                Log("No framebuffer available");
            }
            svcSleepThread(delay);
        }

        Log("Streaming loop ended");
        thread_running = false;
        Log("StreamTaskFunc exiting");
        return 0;
    }

    static void StartStreaming(void)
    {
        Log("StartStreaming called");
        if (thread_running)
        {
            Log("Task already running");
            return;
        }

        int attempt = 0;
        while (streaming_enabled)
        {
            attempt++;
            if (attempt > 1)
            {
                svcSleepThread(1000000000ULL);  // 1s between retries
                if (attempt % 10 == 1)
                {
                    OSD::Notify("Stream: retry #" + std::to_string(attempt));
                    Log("Streaming retry " + std::to_string(attempt));
                }
            }

            // Allocate SOC buffer
            Log("Allocating SOC buffer...");
            socBuffer = (u32*)memalign(0x1000, 0x10000);
            if (!socBuffer)
            {
                Log("memalign failed, retrying...");
                continue;
            }

            // Initialize SOC
            Log("Initializing SOC...");
            Result socRes = socInit(socBuffer, 0x10000);
            if (socRes != 0)
            {
                if (attempt % 10 == 1)
                {
                    OSD::Notify("Stream: SOC busy, waiting...");
                }
                Log("socInit failed: " + std::to_string(socRes));
                free(socBuffer); socBuffer = nullptr;
                continue;
            }
            Log("socInit OK");

            // Resolve host and create UDP socket
            Log("InitConnection...");
            if (!InitConnection())
            {
                Log("InitConnection failed, retrying...");
                socExit();
                free(socBuffer); socBuffer = nullptr;
                continue;
            }
            OSD::Notify("Stream: connected!");
            Log("Connection ready");

            // Start the streaming task
            thread_running = true;
            Log("Creating Task with SysCore affinity");
            stream_task = new Task(StreamTaskFunc, nullptr, Task::Affinity::SysCore);
            int result = stream_task->Start();
            if (result == 0)
            {
                Log("Task started successfully after " + std::to_string(attempt) + " attempts");
                return;  // Success!
            }

            // Task failed to start
            Log("Task start failed: " + std::to_string(result));
            delete stream_task;
            stream_task = nullptr;
            thread_running = false;
            CloseConnection();
            socExit();
            free(socBuffer); socBuffer = nullptr;
            // Loop will retry
        }

        Log("StartStreaming cancelled");
    }

    static void StopStreaming(void)
    {
        Log("StopStreaming called");
        if (!thread_running)
        {
            Log("Task not running");
            return;
        }

        streaming_enabled = false;
        thread_running = false;
        if (stream_task)
        {
            Log("Waiting for task...");
            stream_task->Wait();
            delete stream_task;
            stream_task = nullptr;
            Log("Task completed and freed");
        }

        // Clean up connection and sockets (initialized in main context)
        CloseConnection();
        if (socBuffer)
        {
            Log("Cleaning up sockets...");
            socExit();
            free(socBuffer);
            socBuffer = nullptr;
        }

        streaming_enabled = true;
    }

    // Menu callbacks
    static void ToggleStreaming(MenuEntry *entry)
    {
        if (thread_running)
        {
            StopStreaming();
            OSD::Notify("Streaming stopped");
        }
        else
        {
            StartStreaming();
            OSD::Notify("Streaming started");
        }
    }

    static void ShowConfig(MenuEntry *entry)
    {
        std::string msg = "Host: " + std::string(cfg_host) + "\n";
        msg += "Port: " + std::to_string(cfg_port) + "\n";
        msg += "FPS: " + std::to_string(cfg_fps);
        MessageBox("Cosmic Stream Config", msg)();
    }

    void PatchProcess(FwkSettings &settings)
    {
        Log("=== Cosmic Stream Plugin Starting ===");
        Log("PatchProcess called");
        LoadConfig();
    }

    void OnProcessExit(void)
    {
        Log("OnProcessExit called");
        StopStreaming();
        Log("=== Cosmic Stream Plugin Exiting ===");
    }

    void InitMenu(PluginMenu &menu)
    {
        Log("InitMenu called");
        menu += new MenuEntry("Toggle Streaming", nullptr, ToggleStreaming,
            "Start/stop streaming to Cosmic Unicorn");
        menu += new MenuEntry("Show Config", nullptr, ShowConfig,
            "Show current configuration");
        menu += new MenuEntry("Auto-Start", nullptr, [](MenuEntry *entry)
        {
            StartStreaming();
            OSD::Notify("Auto-streaming enabled");
        }, "Start streaming automatically");
        Log("Menu entries added");
    }

    // OSD callback - runs every frame for reliable notifications
    static bool g_showStartupMsg = true;
    static int g_frameCount = 0;

    static bool OnFrame(const Screen &screen)
    {
        g_frameCount++;

        if (g_frameCount == 1)
        {
            Log("OnFrame first call");
        }

        if (g_showStartupMsg && g_frameCount == 30) // Wait 30 frames for OSD to be ready
        {
            Log("Showing startup message at frame 30");
            OSD::Notify("Cosmic Stream v1.0");
            OSD::Notify("Host: " + std::string(cfg_host) + ":" + std::to_string(cfg_port));
            g_showStartupMsg = false;

            // Start streaming using CTRPluginFramework's Task system
            Log("Starting streaming from OnFrame");
            StartStreaming();
        }

        return true; // Continue callback
    }

    int main(void)
    {
        Log("main() started");

        PluginMenu *menu = new PluginMenu("Cosmic Stream", 1, 0, 0,
            "Stream 3DS screen to Cosmic Unicorn LED panel");
        Log("PluginMenu created");

        menu->SynchronizeWithFrame(true);
        InitMenu(*menu);

        // Register frame callback for reliable OSD
        Log("Registering OSD::Run callback");
        OSD::Run(OnFrame);

        Log("Calling menu->Run()");
        menu->Run();

        Log("menu->Run() returned");
        StopStreaming();
        delete menu;
        Log("main() exiting");
        return 0;
    }
}
