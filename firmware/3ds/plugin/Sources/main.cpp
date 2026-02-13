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
#include <fcntl.h>
#include <errno.h>

// Screen selection
enum ScreenTarget { SCREEN_TOP, SCREEN_BOTTOM };

// Streaming configuration
static char cfg_host[64] = "10.0.0.227";
static int cfg_port = 80;
static int cfg_fps = 20;
static ScreenTarget cfg_screen = SCREEN_TOP;
static bool streaming_enabled = true;
static volatile bool thread_running = false;
static u32 *socBuffer = nullptr;
static bool we_own_soc = false;  // Track if we called socInit (vs piggyback on game)
static bool we_own_ac = false;   // Track if we called acInit
static CTRPluginFramework::Task *stream_task = nullptr;

#define MAX_INIT_RETRIES 5

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
                            else if (strcmp(line, "screen") == 0)
                            {
                                cfg_screen = (strcmp(val, "bottom") == 0 || strcmp(val, "0") == 0) ? SCREEN_BOTTOM : SCREEN_TOP;
                                Log("screen=" + std::string(cfg_screen == SCREEN_TOP ? "top" : "bottom"));
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
        Log("InitConnection: parsing " + std::string(cfg_host));

        memset(&target_addr, 0, sizeof(target_addr));
        target_addr.sin_family = AF_INET;
        target_addr.sin_port = htons(cfg_port);

        // Try parsing as IP address first (faster, no DNS needed)
        in_addr_t ip = inet_addr(cfg_host);
        if (ip != INADDR_NONE)
        {
            target_addr.sin_addr.s_addr = ip;
            Log("Parsed IP directly: " + std::string(cfg_host));
        }
        else
        {
            // Fall back to DNS resolution for hostnames
            Log("Resolving hostname via DNS...");
            struct hostent *host = gethostbyname(cfg_host);
            if (!host)
            {
                Log("DNS resolution failed");
                return false;
            }
            memcpy(&target_addr.sin_addr, host->h_addr, host->h_length);
        }

        Log("Resolved to " + std::string(inet_ntoa(target_addr.sin_addr)));

        udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (udp_sock < 0)
        {
            Log("UDP socket creation failed: " + std::to_string(errno));
            return false;
        }

        // Set socket to non-blocking mode
        int flags = fcntl(udp_sock, F_GETFL, 0);
        if (flags >= 0)
        {
            if (fcntl(udp_sock, F_SETFL, flags | O_NONBLOCK) == 0)
            {
                Log("Set socket to non-blocking");
            }
            else
            {
                Log("Failed to set non-blocking: " + std::to_string(errno));
            }
        }
        else
        {
            Log("Failed to get socket flags: " + std::to_string(errno));
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
    // Runs in background thread so blocking is OK
    static volatile int send_attempt_count = 0;
    static int SendFrame(const u8 *frame)
    {
        if (!connection_ready || udp_sock < 0)
            return -1;

        send_attempt_count++;
        if (send_attempt_count <= 5)
        {
            Log("SendFrame attempt #" + std::to_string(send_attempt_count));
        }

        // Use sendto instead of send - more reliable on 3DS
        int sent = sendto(udp_sock, (const char*)frame, FRAME_SIZE, 0,
                          (struct sockaddr*)&target_addr, sizeof(target_addr));

        if (send_attempt_count <= 5)
        {
            Log("sendto returned: " + std::to_string(sent) + " errno: " + std::to_string(errno));
        }

        if (sent < 0)
        {
            int err = errno;
            return -1000 - err;  // Return negative errno for debugging
        }

        if (sent != FRAME_SIZE)
            return -4;  // Partial send

        return 0;
    }

    // Streaming state - double-buffered to avoid blocking OnFrame
    static int stream_frameCount = 0;
    static int stream_errorCount = 0;
    static int stream_skipFrames = 0;  // How many frames to skip between sends

    // Double buffer: OnFrame writes to one, sender thread reads from other
    static u8 stream_buffer_a[FRAME_SIZE];
    static u8 stream_buffer_b[FRAME_SIZE];
    static volatile u8 *stream_pending = nullptr;  // Points to buffer ready to send
    static volatile bool stream_has_frame = false; // Flag: new frame ready
    static volatile bool sender_running = false;
    static Task *sender_task = nullptr;

    // Background sender thread - sends frames without blocking game
    static s32 SenderTaskFunc(void *arg)
    {
        Log("SenderTaskFunc started");
        u8 local_buffer[FRAME_SIZE];

        while (sender_running)
        {
            // Check stop flag frequently
            if (!sender_running) break;

            // Wait for a frame to be ready
            if (!stream_has_frame)
            {
                svcSleepThread(5000000);  // 5ms sleep to avoid busy-wait
                continue;
            }

            if (!sender_running) break;

            // Copy the pending frame to local buffer
            memcpy(local_buffer, (const void*)stream_pending, FRAME_SIZE);
            stream_has_frame = false;  // Mark as consumed

            if (!sender_running) break;

            // Now send (non-blocking socket, should return immediately)
            int result = SendFrame(local_buffer);

            if (result == 0)
            {
                stream_frameCount++;
                if (stream_frameCount == 1)
                {
                    Log("First frame sent successfully");
                    OSD::Notify("Stream: sending frames!");
                }
                if (stream_frameCount % 300 == 0)
                {
                    OSD::Notify("Stream: " + std::to_string(stream_frameCount) + " frames");
                }
                stream_errorCount = 0;
            }
            else
            {
                stream_errorCount++;
                if (stream_errorCount <= 5)
                {
                    Log("Send error #" + std::to_string(stream_errorCount) + ": " + std::to_string(result));
                }
                if (stream_errorCount % 100 == 0)
                {
                    OSD::Notify("Stream: err " + std::to_string(result), Color::Orange);
                    Log("Send error count: " + std::to_string(stream_errorCount));
                }
            }
        }

        Log("SenderTaskFunc exiting");
        return 0;
    }

    // Called from OnFrame - just captures frame, doesn't block
    static void CaptureFrame(const Screen &screen)
    {
        u8 *fb = (u8*)screen.LeftFramebuffer;
        if (stream_frameCount == 0 && !stream_has_frame)
        {
            Log("CaptureFrame called, fb=" + std::to_string((u32)fb) + " IsTop=" + std::to_string(screen.IsTop));
        }
        if (!fb)
        {
            return;
        }

        // If previous frame hasn't been sent yet, skip (drop frame)
        if (stream_has_frame)
        {
            return;
        }

        // Pick which buffer to write to (alternate)
        static bool use_buffer_a = true;
        u8 *dst = use_buffer_a ? stream_buffer_a : stream_buffer_b;
        use_buffer_a = !use_buffer_a;

        // Top screen is 400x240, bottom screen is 320x240
        int srcW = screen.IsTop ? 400 : 320;
        DownsampleFrame(fb, srcW, 240, dst);

        // Make frame available to sender
        stream_pending = dst;
        stream_has_frame = true;
    }

    // Try to initialize sockets - either use game's existing sockets or init our own
    static bool InitSockets(void)
    {
        // First, test if sockets are already available (game initialized them)
        Log("Testing if sockets already available...");
        int testSock = socket(AF_INET, SOCK_DGRAM, 0);
        if (testSock >= 0)
        {
            // Game already initialized sockets - we can piggyback!
            closesocket(testSock);
            Log("Sockets already available (game initialized) - using existing");
            OSD::Notify("Stream: using game sockets");
            we_own_soc = false;
            we_own_ac = false;
            return true;
        }

        // Sockets not available - we need to init ourselves
        Log("Sockets not available, initializing our own...");

        // First init AC (WiFi service) - required before socInit
        Result acRes = acInit();
        if (acRes != 0)
        {
            Log("acInit failed: " + std::to_string(acRes));
            // Try anyway, game might have AC initialized
        }
        else
        {
            Log("acInit OK");
            we_own_ac = true;
        }

        // Check if WiFi is connected
        u32 wifiStatus = 0;
        if (ACU_GetWifiStatus(&wifiStatus) == 0)
        {
            Log("WiFi status: " + std::to_string(wifiStatus));
            if (wifiStatus == 0)
            {
                Log("WiFi not connected");
                OSD::Notify("Stream: no WiFi!", Color::Orange);
                if (we_own_ac) { acExit(); we_own_ac = false; }
                return false;
            }
        }

        // Use 128KB buffer like official examples
        socBuffer = (u32*)memalign(0x1000, 0x20000);
        if (!socBuffer)
        {
            Log("memalign failed");
            if (we_own_ac) { acExit(); we_own_ac = false; }
            return false;
        }

        Result socRes = socInit(socBuffer, 0x20000);
        if (socRes != 0)
        {
            Log("socInit failed: " + std::to_string(socRes));
            free(socBuffer);
            socBuffer = nullptr;
            if (we_own_ac) { acExit(); we_own_ac = false; }
            return false;
        }

        Log("socInit OK - we own the sockets");
        OSD::Notify("Stream: initialized sockets");
        we_own_soc = true;
        return true;
    }

    // Background init task - runs socket init without blocking main thread
    static volatile bool init_in_progress = false;
    static volatile bool init_succeeded = false;

    static s32 InitTaskFunc(void *arg)
    {
        Log("InitTaskFunc started");

        for (int attempt = 1; attempt <= MAX_INIT_RETRIES; attempt++)
        {
            if (attempt > 1)
            {
                // Exponential backoff: 1s, 2s, 4s, 8s...
                u64 delay = (1ULL << (attempt - 1)) * 1000000000ULL;
                if (delay > 8000000000ULL) delay = 8000000000ULL;  // Cap at 8s
                Log("Retry " + std::to_string(attempt) + ", waiting " + std::to_string(delay / 1000000000ULL) + "s");
                OSD::Notify("Stream: retry " + std::to_string(attempt) + "/" + std::to_string(MAX_INIT_RETRIES));
                svcSleepThread(delay);
            }

            // Initialize sockets (detect game's or init our own)
            if (!InitSockets())
            {
                Log("InitSockets failed on attempt " + std::to_string(attempt));
                continue;
            }

            // Resolve host and create UDP socket
            Log("InitConnection...");
            if (!InitConnection())
            {
                Log("InitConnection failed, cleaning up...");
                if (we_own_soc)
                {
                    socExit();
                    free(socBuffer);
                    socBuffer = nullptr;
                    we_own_soc = false;
                }
                if (we_own_ac)
                {
                    acExit();
                    we_own_ac = false;
                }
                continue;
            }

            // Success! Start background sender thread
            OSD::Notify("Stream: connected!");
            Log("Connection ready - starting sender thread");

            // Calculate frame skip for target FPS (game runs at ~60fps)
            stream_skipFrames = (60 / cfg_fps) - 1;
            if (stream_skipFrames < 0) stream_skipFrames = 0;
            Log("Frame skip: " + std::to_string(stream_skipFrames) + " (target " + std::to_string(cfg_fps) + " fps)");

            // Start sender thread
            sender_running = true;
            sender_task = new Task(SenderTaskFunc, nullptr, Task::Affinity::SysCore);
            sender_task->Start();
            Log("Sender task started");

            thread_running = true;  // Signal that streaming is active
            init_succeeded = true;
            init_in_progress = false;

            return 0;
        }

        // All retries exhausted
        Log("Init failed after " + std::to_string(MAX_INIT_RETRIES) + " attempts");
        OSD::Notify("Stream: init failed!", Color::Red);
        init_in_progress = false;
        return -1;
    }

    static Task *init_task = nullptr;

    static void StartStreaming(void)
    {
        Log("StartStreaming called");
        if (thread_running || init_in_progress)
        {
            Log("Already running or init in progress");
            return;
        }

        init_in_progress = true;
        init_succeeded = false;
        stream_frameCount = 0;
        stream_errorCount = 0;

        // Run init in background so we don't block the game
        Log("Starting background init task");
        if (init_task)
        {
            delete init_task;
        }
        init_task = new Task(InitTaskFunc, nullptr, Task::Affinity::SysCore);
        init_task->Start();
        // Task runs in background - we don't wait for it
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

        // Stop sender thread
        if (sender_task)
        {
            Log("Stopping sender task...");
            sender_running = false;
            sender_task->Wait();
            delete sender_task;
            sender_task = nullptr;
            Log("Sender task stopped");
        }

        if (stream_task)
        {
            Log("Waiting for task...");
            stream_task->Wait();
            delete stream_task;
            stream_task = nullptr;
            Log("Task completed and freed");
        }

        // Clean up connection
        CloseConnection();

        // Only cleanup sockets if we initialized them (not if piggybacking on game)
        if (we_own_soc && socBuffer)
        {
            Log("Cleaning up our sockets...");
            socExit();
            free(socBuffer);
            socBuffer = nullptr;
            we_own_soc = false;
        }
        else
        {
            Log("Not cleaning sockets (using game's)");
        }

        // Cleanup AC if we initialized it
        if (we_own_ac)
        {
            Log("Cleaning up AC...");
            acExit();
            we_own_ac = false;
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

    // OSD callback - runs every frame for reliable notifications AND streaming
    static bool g_showStartupMsg = true;
    static int g_frameCount = 0;
    static int g_streamSkipCounter = 0;

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

            // Start connection init in background
            Log("Starting streaming from OnFrame");
            StartStreaming();
        }

        // If streaming is active and connected, capture frames for sender thread
        // The 'screen' parameter passed to callback IS the framebuffer to use
        if (thread_running && connection_ready)
        {
            // Only stream if this is the screen we want (top or bottom)
            bool isTargetScreen = (cfg_screen == SCREEN_TOP) ? screen.IsTop : !screen.IsTop;
            if (!isTargetScreen)
            {
                return true;  // Not the screen we want, skip
            }

            g_streamSkipCounter++;
            // Skip frames to match target FPS (game runs at ~60fps)
            if (g_streamSkipCounter > stream_skipFrames)
            {
                g_streamSkipCounter = 0;
                CaptureFrame(screen);  // Just captures, doesn't block
            }
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
