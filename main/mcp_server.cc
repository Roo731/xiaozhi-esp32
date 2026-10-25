/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

#include "mcp_server.h"
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_pthread.h>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <optional>

#include "application.h"
#include "board.h"
#include "settings.h"

#if defined(ESP_PLATFORM) && defined(CONFIG_IDF_TARGET_ESP32S3)
#include <esp_now.h>
#include <esp_wifi.h>
#include <driver/ledc.h>
#endif

#define TAG "MCP"

#if defined(ESP_PLATFORM) && defined(CONFIG_IDF_TARGET_ESP32S3)
// 📌 आपका दिया गया असली मैक एड्रेस
uint8_t slave_mac[] = {0x28, 0x84, 0x85, 0x8A, 0x71, 0x28}; 

// ESP-NOW इनिशियलाइजेशन फंक्शन
void InitAnimaEspNow() {
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    if (esp_now_init() != ESP_OK) {
        ESP_LOGE(TAG, "ESP-NOW Initialization Failed!");
        return;
    }

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, slave_mac, 6);
    peerInfo.channel = 0;  
    peerInfo.encrypt = false;

    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add peer device!");
        return;
    }
    ESP_LOGI(TAG, "Anima-6 Slave Board paired successfully via ESP-NOW!");
}

// दूसरे बोर्ड को कमांड भेजने वाला फंक्शन
void SendAnimaCommand(const char* action) {
    esp_err_t result = esp_now_send(slave_mac, (uint8_t*)action, strlen(action) + 1);
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Sent Anima command via ESP-NOW: %s", action);
    } else {
        ESP_LOGE(TAG, "Error sending ESP-NOW command: %s", action);
    }
}

// 📌 GPIO 21 के लिए सर्वो मोटर शटर क्लास (कैमरा गेट) - h_output_en एरर हटाया गया
class ServoShutter {
private:
    int gpio_num_;
    bool initialized_;

    void SetAngle(int angle) {
        uint32_t duty = 205 + (angle * (1024 - 205) / 180);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }

public:
    ServoShutter(int gpio) : gpio_num_(gpio), initialized_(false) {
        ledc_timer_config_t ledc_timer = {
            .speed_mode       = LEDC_LOW_SPEED_MODE,
            .duty_resolution  = LEDC_TIMER_13_BIT,
            .timer_num        = LEDC_TIMER_0,
            .freq_hz          = 50,
            .clk_cfg          = LEDC_AUTO_CLK
        };
        if (ledc_timer_config(&ledc_timer) != ESP_OK) return;

        ledc_channel_config_t ledc_channel = {
            .gpio_num       = gpio_num_,
            .speed_mode     = LEDC_LOW_SPEED_MODE,
            .channel        = LEDC_CHANNEL_0,
            .intr_type      = LEDC_INTR_DISABLE,
            .timer_sel      = LEDC_TIMER_0,
            .duty           = 0
        };
        if (ledc_channel_config(&ledc_channel) == ESP_OK) {
            initialized_ = true;
        }
    }

    void Open() {
        if (!initialized_) return;
        SetAngle(180); // गेट खोलें (180°)
        vTaskDelay(pdMS_TO_TICKS(400)); 
    }

    void Close() {
        if (!initialized_) return;
        SetAngle(0); // गेट बंद करें (0°)
        vTaskDelay(pdMS_TO_TICKS(400)); 
    }
};

struct ServoShutterGuard {
    ServoShutter& shutter;
    ServoShutterGuard(ServoShutter& s) : shutter(s) {
        shutter.Open();
    }
    ~ServoShutterGuard() {
        shutter.Close();
    }
};
#endif

McpServer::McpServer() {
#if defined(ESP_PLATFORM) && defined(CONFIG_IDF_TARGET_ESP32S3)
    InitAnimaEspNow();
#endif
}

McpServer::~McpServer() = default;

void McpServer::AddCommonTools() {
    auto original_tools = std::move(tools_);
    auto& board = Board::GetInstance();

    AddTool("self.get_device_status",
            "Provides the real-time information of the device, including the current status of the "
            "audio speaker, screen, battery, network, etc.",
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

    // 1. एनिमा बॉडी मूवमेंट कमांड्स (ESP-NOW के जरिए दूसरे बोर्ड को भेजने के लिए)
    AddTool("control_anima_body", 
        "Control Anima 6 robot body movements and postures. Pass one action string in 'action': "
        "namaste, raise_right_hand, raise_left_hand, turn_left, turn_right, look_up, look_down, "
        "look_left, look_right, scary_mode, reset_pose", 
        PropertyList({Property("action", kPropertyTypeString)}),
        [](const PropertyList& properties) -> ReturnValue {
            auto action = properties["action"].value<std::string>();
#if defined(ESP_PLATFORM) && defined(CONFIG_IDF_TARGET_ESP32S3)
            SendAnimaCommand(action.c_str());
#endif
            return true;
        }
    );

    // 2. ऑटो सर्वो गेट कंट्रोल वाला कैमरा टूल
    auto camera = board.GetCamera();
    if (camera) {
        AddTool("self.camera.take_photo",
                "Take a photo using the camera and explain it. Opens servo gate before and closes after.",
                PropertyList({Property("question", kPropertyTypeString)}),
                [camera](const PropertyList& properties) -> ToolResult {
                    TaskPriorityReset priority_reset(1);

#if defined(ESP_PLATFORM) && defined(CONFIG_IDF_TARGET_ESP32S3)
                    static ServoShutter shutter(21);
                    ServoShutterGuard guard(shutter);
#endif

                    if (!camera->Capture()) {
                        return std::unexpected("Failed to capture photo");
                    }
                    auto question = properties["question"].value<std::string>();
                    auto result = camera->Explain(question);
                    if (!result) {
                        return std::unexpected(std::move(result.error()));
                    }
                    return std::move(*result);
                });
    }

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

void McpServer::ParseCapabilities(const cJSON* capabilities) {
    auto vision = cJSON_GetObjectItem(capabilities, "vision");
    if (cJSON_IsObject(vision)) {
        auto url = cJSON_GetObjectItem(vision, "url");
        auto token = cJSON_GetObjectItem(vision, "token");
        if (cJSON_IsString(url)) {
            auto camera = Board::GetInstance().GetCamera();
            if (camera) {
                std::string url_str = std::string(url->valuestring);
                std::string token_str;
                if (cJSON_IsString(token)) {
                    token_str = std::string(token->valuestring);
                }
                camera->SetExplainUrl(url_str, token_str);
            }
        }
    }
}

void McpServer::ParseMessage(const cJSON* json, ResponseSender response_sender) {
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0) return;

    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method)) return;

    std::string method_str(method->valuestring);
    if (method_str.find("notifications") == 0) return;

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id)) return;
    int id_int = id->valueint;

    if (method_str == "initialize") {
        auto app_desc = esp_app_get_description();
        std::string message = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"";
        message += app_desc->version;
        message += "\"}}";
        ReplyResult(id_int, message, response_sender);
    } else if (method_str == "tools/list") {
        GetToolsList(id_int, "", false, response_sender);
    } else if (method_str == "tools/call") {
        auto params = cJSON_GetObjectItem(json, "params");
        auto tool_name = cJSON_GetObjectItem(params, "name");
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments, std::move(response_sender));
    } else {
        ReplyError(id_int, -32601, "Method not implemented", response_sender);
    }
}

void McpServer::SendResponse(const std::string& payload, const ResponseSender& response_sender) {
    if (response_sender) {
        response_sender(payload);
    } else {
        Application::GetInstance().SendMcpMessage(payload);
    }
}

void McpServer::ReplyResult(id, const std::string& result, const ResponseSender& response_sender) {} // Handled inline below
void McpServer::ReplyResult(int id, const std::string& result, const ResponseSender& response_sender) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"result\":" + result + "}";
    SendResponse(payload, response_sender);
}

void McpServer::ReplyError(int id, int code, const std::string& message, const ResponseSender& response_sender) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"error\":{\"code\":" + std::to_string(code) + ",\"message\":\"" + message + "\"}}";
    SendResponse(payload, response_sender);
}

void McpServer::GetToolsList(int id, const std::string& cursor, bool list_user_only_tools, const ResponseSender& response_sender) {
    std::string json = "{\"tools\":[";
    for (auto it = tools_.begin(); it != tools_.end(); ++it) {
        if (!list_user_only_tools && (*it)->user_only()) continue;
        json += (*it)->to_json() + ",";
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
        ReplyError(id, -32602, "Unknown tool", response_sender);
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
