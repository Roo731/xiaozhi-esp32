/*
 * MCP Server Implementation - (XiaoZhi AI + OV3660 Camera + Servo Gate)
 */

#include "mcp_server.h"
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_pthread.h>
#include <algorithm>
#include <cstring>
#include <iterator>

#include "application.h"
#include "board.h"
#include "display.h"
#include "lvgl_image.h"
#include "lvgl_theme.h"
#include "settings.h"

#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <driver/ledc.h>

#define TAG "MCP"

#define SERVO_PIN         21          
#define SERVO_LEDC_CHANNEL LEDC_CHANNEL_1
#define SERVO_LEDC_TIMER   LEDC_TIMER_1
#define SERVO_FREQ_HZ     50          
#define SERVO_MIN_PULSE   500         
#define SERVO_MAX_PULSE   2500        

void SetServoAngle(int angle) {
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;

    uint32_t duty = (SERVO_MIN_PULSE + ((SERVO_MAX_PULSE - SERVO_MIN_PULSE) * angle) / 180);
    uint32_t ledc_duty = (duty * 16383) / 20000; 

    ledc_set_duty(LEDC_LOW_SPEED_MODE, SERVO_LEDC_CHANNEL, ledc_duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, SERVO_LEDC_CHANNEL);
}

void InitServo() {
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_14_BIT,
        .timer_num = SERVO_LEDC_TIMER,
        .freq_hz = SERVO_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer_conf);

    ledc_channel_config_t channel_conf = {
        .gpio_num = SERVO_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = SERVO_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = SERVO_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&channel_conf);

    SetServoAngle(0);
}

// ==================== OV3660 कैमरा पिन कॉन्फ़िगरेशन ====================
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     15
#define SIOD_GPIO_NUM     4
#define SIOC_GPIO_NUM     5

#define Y9_GPIO_NUM       16
#define Y8_GPIO_NUM       17
#define Y7_GPIO_NUM       18
#define Y6_GPIO_NUM       12
#define Y5_GPIO_NUM       10
#define Y4_GPIO_NUM       8
#define Y3_GPIO_NUM       9
#define Y2_GPIO_NUM       11

#define VSYNC_GPIO_NUM    6
#define HREF_GPIO_NUM     7
#define PCLK_GPIO_NUM     13

void RegisterAnimaBodyControl(McpServer* server);

McpServer::McpServer() {}
McpServer::~McpServer() = default;

void McpServer::AddCommonTools() {
    auto original_tools = std::move(tools_);
    auto& board = Board::GetInstance();

    RegisterAnimaBodyControl(this);

    AddTool("self.get_device_status",
            "Provides real-time information of the device.",
            PropertyList(), [&board](const PropertyList& properties) -> ReturnValue {
                return board.GetDeviceStatusJson();
            });

    AddTool("self.audio_speaker.set_volume",
            "Set the volume of the audio speaker.",
            PropertyList({Property("volume", kPropertyTypeInteger, 0, 100)}),
            [&board](const PropertyList& properties) -> ReturnValue {
                auto codec = board.GetAudioCodec();
                codec->SetOutputVolume(properties["volume"].value<int>());
                return true;
            });

    tools_.insert(tools_.end(), std::make_move_iterator(original_tools.begin()),
                  std::make_move_iterator(original_tools.end()));
}

void McpServer::AddUserOnlyTools() {
    AddUserOnlyTool("self.get_system_info", "Get the system information", PropertyList(),
                    [this](const PropertyList& properties) -> ReturnValue {
                        auto& board = Board::GetInstance();
                        return board.GetSystemInfoJson();
                    });

    AddUserOnlyTool("self.reboot", "Reboot the system", PropertyList(),
                    [this](const PropertyList& properties) -> ReturnValue {
                        auto& app = Application::GetInstance();
                        app.Schedule([&app]() {
                            ESP_LOGW(TAG, "User requested reboot");
                            vTaskDelay(pdMS_TO_TICKS(1000));
                            app.Reboot();
                        });
                        return true;
                    });
}

void McpServer::AddTool(std::unique_ptr<McpTool> tool) {
    if (std::find_if(tools_.begin(), tools_.end(), [&tool](const auto& existing) {
            return existing->name() == tool->name();
        }) != tools_.end()) {
        return;
    }
    tools_.push_back(std::move(tool));
}

void McpServer::AddTool(const std::string& name, const std::string& description,
                        const PropertyList& properties, ToolCallback callback) {
    AddTool(std::make_unique<McpTool>(name, description, properties, std::move(callback)));
}

void McpServer::AddUserOnlyTool(const std::string& name, const std::string& description,
                                const PropertyList& properties, ToolCallback callback) {
    auto tool = std::make_unique<McpTool>(name, description, properties, std::move(callback));
    tool->set_user_only(true);
    AddTool(std::move(tool));
}

void McpServer::ParseMessage(const std::string& message, ResponseSender response_sender) {
    CJsonUniquePtr json(cJSON_Parse(message.c_str()));
    if (json == nullptr) return;
    ParseMessage(json.get(), std::move(response_sender));
}

void McpServer::ParseCapabilities(const cJSON* capabilities) {}

void McpServer::ParseMessage(const cJSON* json, ResponseSender response_sender) {
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0) return;

    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method)) return;

    auto method_str = std::string(method->valuestring);
    if (method_str.find("notifications") == 0) return;

    auto params = cJSON_GetObjectItem(json, "params");
    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id)) return;
    auto id_int = id->valueint;

    if (method_str == "initialize") {
        auto app_desc = esp_app_get_description();
        std::string message = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"" + std::string(app_desc->version) + "\"}}";
        ReplyResult(id_int, message, response_sender);
    } else if (method_str == "tools/list") {
        GetToolsList(id_int, "", false, response_sender);
    } else if (method_str == "tools/call") {
        if (!cJSON_IsObject(params)) return;
        auto tool_name = cJSON_GetObjectItem(params, "name");
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_name && cJSON_IsString(tool_name)) {
            DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments, std::move(response_sender));
        }
    }
}

void McpServer::SendResponse(const std::string& payload, const ResponseSender& response_sender) {
    if (response_sender) response_sender(payload);
    else Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::ReplyResult(int id, const std::string& result, const ResponseSender& response_sender) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"result\":" + result + "}";
    SendResponse(payload, response_sender);
}

void McpServer::ReplyError(int id, int code, const std::string& message, const ResponseSender& response_sender) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"error\":{\"code\":" + std::to_string(code) + ",\"message\":\"" + message + "\"}}";
    SendResponse(payload, response_sender);
}

void McpServer::ReplyError(int id, const std::string& message, const ResponseSender& response_sender) {
    ReplyError(id, -32603, message, response_sender);
}

void McpServer::GetToolsList(int id, const std::string& cursor, bool list_user_only_tools, const ResponseSender& response_sender) {
    std::string json = "{\"tools\":[";
    for (size_t i = 0; i < tools_.size(); ++i) {
        if (!list_user_only_tools && tools_[i]->user_only()) continue;
        json += tools_[i]->to_json();
        if (i < tools_.size() - 1) json += ",";
    }
    if (json.back() == ',') json.pop_back();
    json += "]}";
    ReplyResult(id, json, response_sender);
}

void McpServer::DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments, ResponseSender response_sender) {
    auto tool_iter = std::find_if(tools_.begin(), tools_.end(), [&tool_name](const auto& tool) {
        return tool->name() == tool_name;
    });

    if (tool_iter == tools_.end()) {
        ReplyError(id, -32602, "Unknown tool: " + tool_name, response_sender);
        return;
    }

    McpTool* tool = tool_iter->get();
    PropertyList arguments = tool->properties();

    auto& app = Application::GetInstance();
    app.Schedule([this, id, tool, arguments = std::move(arguments), response_sender = std::move(response_sender)]() {
        auto result = tool->Call(arguments);
        if (!result) {
            ReplyError(id, result.error(), response_sender);
            return;
        }
        ReplyResult(id, *result, response_sender);
    });
}

// ==================== एनिमा ESP-NOW और असिस्टेंट मोड ====================
static const char* ANIMA_TAG = "ANIMA_BOARD1";
static uint8_t board2_mac[6] = {0x28, 0x84, 0x85, 0x8A, 0x71, 0x28};
static bool g_assistant_mode_active = true;

static void OnDataRecv(const esp_now_recv_info_t *esp_now_info, const uint8_t *incomingData, int len) {
    char message[32] = {0};
    int copy_len = (len < sizeof(message) - 1) ? len : sizeof(message) - 1;
    memcpy(message, incomingData, copy_len);
    message[copy_len] = '\0';

    if (strcmp(message, "ASSISTANT_ON") == 0) {
        g_assistant_mode_active = true;
    } else if (strcmp(message, "ASSISTANT_OFF") == 0) {
        g_assistant_mode_active = false;
    }
}

void SendAnimaCommand(const char* action_cmd) {
    if (!g_assistant_mode_active && strcmp(action_cmd, "FORCE") != 0) return;
    char packet[64];
    snprintf(packet, sizeof(packet), "CMD:%s", action_cmd);
    esp_now_send(board2_mac, (uint8_t *)packet, strlen(packet));
}

// Wi-Fi कनेक्शन स्टेटस टास्क (ESP-IDF कंपेटिबल)
static void WiFiStatusTask(void* pvParameters) {
    wifi_ap_record_t ap_info;
    char wifi_packet[64];

    while (1) {
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            int rssi = ap_info.rssi;
            int quality = (rssi <= -100) ? 0 : (rssi >= -50 ? 100 : 2 * (rssi + 100));
            snprintf(wifi_packet, sizeof(wifi_packet), "WIFI:CON:%d:%d%%", rssi, quality);
        } else {
            snprintf(wifi_packet, sizeof(wifi_packet), "WIFI:DIS");
        }

        esp_now_send(board2_mac, (uint8_t*)wifi_packet, strlen(wifi_packet));
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void Anima_Camera_Init() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;

    if (psramFound()) {
        config.frame_size = FRAMESIZE_VGA;
        config.jpeg_quality = 12;          
        config.fb_count = 2;
    } else {
        config.frame_size = FRAMESIZE_QVGA;
        config.jpeg_quality = 12;
        config.fb_count = 1;
    }

    esp_camera_init(&config);
}

void Anima_ESPNow_Init() {
    if (esp_now_init() != ESP_OK) return;
    esp_now_register_recv_cb(OnDataRecv);

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, board2_mac, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;

    if (esp_now_add_peer(&peerInfo) != ESP_OK) return;
    xTaskCreate(WiFiStatusTask, "wifi_status_task", 3072, NULL, 5, NULL);
}

// ==================== MCP टूल रजिस्ट्रेशन ====================
void RegisterAnimaBodyControl(McpServer* server) {
    InitServo();           
    Anima_ESPNow_Init();   
    Anima_Camera_Init();   

    // 1. एनिमा बॉडी मूवमेंट कमांड्स
    server->AddTool("control_anima_body", 
        "Control Anima 6 robot body movements and postures. Pass one of the following exact action strings in the 'action' parameter:\n"
        "- 'namaste': Fold hands together in front to greet/namaste\n"
        "- 'raise_right_hand': Raise the right arm/hand up\n"
        "- 'raise_left_hand': Raise the left arm/hand up\n"
        "- 'turn_left': Rotate body base turn towards left\n"
        "- 'turn_right': Rotate body base turn towards right\n"
        "- 'look_up': Tilt head up\n"
        "- 'look_down': Tilt head down\n"
        "- 'look_left': Rotate head pan to left\n"
        "- 'look_right': Rotate head pan to right\n"
        "- 'scary_mode': Activate scary posture with raised arms and wide eyes\n"
        "- 'reset_pose': Return all servos and head to default resting position", 
        PropertyList({Property("action", kPropertyTypeString)}),
        [](const PropertyList& properties) -> ReturnValue {
            auto action = properties["action"].value<std::string>();
            SendAnimaCommand(action.c_str());
            return true;
        }
    );

    // 2. ऑटो सर्वो गेट कंट्रोल (0° -> 180° -> Photo -> 0°) वाला कैमरा टूल
    server->AddTool("self.camera.take_photo",
        "Take a snapshot/photo using the OV3660 camera. Opens the servo gate before taking the photo and closes it afterwards.",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            // गेट खोलें (180°)
            SetServoAngle(180);
            vTaskDelay(pdMS_TO_TICKS(500)); 

            // फोटो लें
            camera_fb_t * fb = esp_camera_fb_get();
            if (!fb) {
                SetServoAngle(0);
                return "Error: Camera capture failed";
            }
            
            esp_camera_fb_return(fb);

            // गेट बंद करें (0°)
            vTaskDelay(pdMS_TO_TICKS(200));
            SetServoAngle(0);
            
            return "Photo captured successfully. Ready for visual inspection.";
        }
    );
}
