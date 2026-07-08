#include "plugin.hpp"
#include <zmq.hpp>
#include <thread>
#include <mutex>
#include <atomic>

// Структура пакета данных
struct ZmqAudioFrame {
    float cv[9];
    float midi_pitch[16];
    float midi_gate[16];
    float midi_vel[16];
};

struct ZeroMQSocket : Module {
    enum ParamId { PORT_PARAM, PROTO_PARAM, BP_PARAM, PARAMS_LEN };
    enum InputId { INPUTS_LEN };
    enum OutputId {
        CV_OUT_1, CV_OUT_2, CV_OUT_3, CV_OUT_4, CV_OUT_5, CV_OUT_6, CV_OUT_7, CV_OUT_8, CV_OUT_9,
        POLY_PITCH_OUT, POLY_GATE_OUT, POLY_VEL_OUT,
        OUTPUTS_LEN
    };
    enum LightId { LIGHTS_LEN };

    // Сетевые потоки и буферизация
    std::thread* workerThread = nullptr;
    std::mutex bufferMutex;
    std::atomic<bool> threadRunning{false};
    zmq::context_t* context = nullptr;
    zmq::socket_t* socket = nullptr;
    
    // Двойной буфер для потокобезопасности
    ZmqAudioFrame activeFrame{};
    ZmqAudioFrame backFrame{};
    std::atomic<bool> newFrameAvailable{false};

    // Таймер отсутствия сетевой активности
    std::atomic<float> timeSinceLastPacket;

    int port = 5555;
    int currentActivePort = 5555;
    int currentActiveProto = 0; // 0 = PULL, 1 = SUB

    ZeroMQSocket() {
        timeSinceLastPacket.store(99.f);
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        
        configParam(PORT_PARAM, 1024.f, 65535.f, 5555.f, "ZMQ Port");
        configParam(PROTO_PARAM, 0.f, 1.f, 0.f, "Protocol (PULL/SUB)");
        configParam(BP_PARAM, 0.f, 1.f, 0.f, "Bypass");
        
        // Конфигурация портов
        for (int i = 0; i < 9; i++) {
            configOutput(CV_OUT_1 + i, "CV Channel " + std::to_string(i + 1));
        }
        configOutput(POLY_PITCH_OUT, "Polyphonic Pitch (V/OCT)");
        configOutput(POLY_GATE_OUT, "Polyphonic Gate");
        configOutput(POLY_VEL_OUT, "Polyphonic Velocity");

        // Запуск фонового сетевого потока
        port = (int)params[PORT_PARAM].getValue();
        currentActivePort = port;
        currentActiveProto = (int)params[PROTO_PARAM].getValue();
        startWorker();
    }

    ~ZeroMQSocket() {
        stopWorker();
    }

    void startWorker() {
        threadRunning = true;
        int bindPort = currentActivePort;
        int bindProto = currentActiveProto;
        
        try {
            context = new zmq::context_t(1);
            socket = new zmq::socket_t(*context, bindProto == 1 ? zmq::socket_type::sub : zmq::socket_type::pull);
            
            zmq::socket_t* localSocket = socket;
            
            workerThread = new std::thread([this, localSocket, bindPort, bindProto]() {
                try {
                    std::string bindAddress = "tcp://*:" + std::to_string(bindPort);
                    localSocket->bind(bindAddress);
                    localSocket->set(zmq::sockopt::rcvtimeo, 500); // Таймаут чтения 500мс
                    
                    if (bindProto == 1) {
                        localSocket->set(zmq::sockopt::subscribe, "");
                    }
                    
                    while (threadRunning) {
                        zmq::message_t message;
                        auto result = localSocket->recv(message, zmq::recv_flags::none);
                        
                        if (result && message.size() == sizeof(ZmqAudioFrame)) {
                            // Быстро копируем данные в фоновый буфер
                            std::lock_guard<std::mutex> lock(bufferMutex);
                            std::memcpy(&backFrame, message.data(), sizeof(ZmqAudioFrame));
                            newFrameAvailable = true;
                            timeSinceLastPacket.store(0.f); // Сбрасываем таймер активности
                        }
                    }
                } catch (const std::exception& e) {
                    DEBUG("ZeroMQ Error on port %d (proto %d): %s", bindPort, bindProto, e.what());
                }
            });
        } catch (const std::exception& e) {
            DEBUG("ZeroMQ Thread Init Error on port %d (proto %d): %s", bindPort, bindProto, e.what());
        }
    }

    void stopWorker() {
        threadRunning = false;
        if (socket) {
            try {
                socket->close();
            } catch (...) {}
        }
        if (context) {
            try {
                context->close();
            } catch (...) {}
        }
        if (workerThread) {
            if (workerThread->joinable()) {
                workerThread->join();
            }
            delete workerThread;
            workerThread = nullptr;
        }
        if (socket) {
            delete socket;
            socket = nullptr;
        }
        if (context) {
            delete context;
            context = nullptr;
        }
    }

    void process(const ProcessArgs& args) override {
        // Динамический перезапуск при изменении порта или протокола
        int targetPort = (int)params[PORT_PARAM].getValue();
        int targetProto = (int)params[PROTO_PARAM].getValue();
        if (targetPort != currentActivePort || targetProto != currentActiveProto) {
            stopWorker();
            currentActivePort = targetPort;
            currentActiveProto = targetProto;
            startWorker();
        }

        // Обновляем таймер неактивности
        timeSinceLastPacket.store(timeSinceLastPacket.load() + args.sampleTime);

        // Проверяем наличие новых данных от ZMQ воркера
        if (newFrameAvailable) {
            if (bufferMutex.try_lock()) {
                activeFrame = backFrame;
                newFrameAvailable = false;
                bufferMutex.unlock();
            }
        }

        // Проверяем статус байпаса
        bool bypassed = (params[BP_PARAM].getValue() > 0.5f);

        // 1. Вывод моно-сигналов CV (0-9)
        for (int i = 0; i < 9; i++) {
            outputs[CV_OUT_1 + i].setVoltage(bypassed ? 0.f : activeFrame.cv[i]);
        }

        // 2. Вывод полифонических MIDI/CV сигналов
        outputs[POLY_PITCH_OUT].setChannels(16);
        outputs[POLY_GATE_OUT].setChannels(16);
        outputs[POLY_VEL_OUT].setChannels(16);

        for (int c = 0; c < 16; c++) {
            outputs[POLY_PITCH_OUT].setVoltage(bypassed ? 0.f : activeFrame.midi_pitch[c], c);
            outputs[POLY_GATE_OUT].setVoltage(bypassed ? 0.f : activeFrame.midi_gate[c], c);
            outputs[POLY_VEL_OUT].setVoltage(bypassed ? 0.f : activeFrame.midi_vel[c], c);
        }
    }
};

// Forward declarations
struct ZeroMQSocketWidget;

// Виджет OLED-дисплея
struct OLEDDisplay : TransparentWidget {
    std::shared_ptr<Font> font;

    OLEDDisplay() {
        box.size = Vec(120.f, 70.f);
        font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
    }

    void draw(const DrawArgs& args) override;
};

struct ZeroMQSocketWidget : ModuleWidget {
    ZeroMQSocketWidget(ZeroMQSocket* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/ZeroMQSocket.svg")));

        // Винты
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // OLED Дисплей (размещение на x = 15px (5.08mm), y = 30px (10mm))
        OLEDDisplay* display = createWidget<OLEDDisplay>(mm2px(Vec(5.08, 10.0)));
        addChild(display);

        // Ручка порта (x = 12.7mm, y = 52.0mm)
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.7, 52.0)), module, ZeroMQSocket::PORT_PARAM));
        
        // Переключатель протокола PULL/SUB (x = 25.4mm, y = 52.0mm)
        addParam(createParamCentered<CKSS>(mm2px(Vec(25.4, 52.0)), module, ZeroMQSocket::PROTO_PARAM));
        
        // Переключатель байпаса ON/OFF (x = 38.1mm, y = 52.0mm)
        addParam(createParamCentered<CKSS>(mm2px(Vec(38.1, 52.0)), module, ZeroMQSocket::BP_PARAM));

        // 9 моно выходов CV в сетке 3x3
        // Колонки: x = 12.7mm, 25.4mm, 38.1mm
        // Ряды: y = 65.0mm, 77.0mm, 89.0mm
        for (int i = 0; i < 9; i++) {
            int row = i / 3;
            int col = i % 3;
            float x = 12.7f + col * 12.7f;
            float y = 65.0f + row * 12.0f;
            addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x, y)), module, ZeroMQSocket::CV_OUT_1 + i));
        }

        // Выходы MIDI Poly (в один ряд: x = 12.7mm, 25.4mm, 38.1mm, y = 110mm)
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(12.7, 110.0)), module, ZeroMQSocket::POLY_PITCH_OUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(25.4, 110.0)), module, ZeroMQSocket::POLY_GATE_OUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(38.1, 110.0)), module, ZeroMQSocket::POLY_VEL_OUT));
    }
};

void OLEDDisplay::draw(const DrawArgs& args) {
    // Фоновая заливка экрана (тёмно-синий OLED-цвет)
    nvgBeginPath(args.vg);
    nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 4.f);
    nvgFillColor(args.vg, nvgRGBA(10, 13, 20, 255));
    nvgFill(args.vg);

    // Рамка экрана
    nvgStrokeColor(args.vg, nvgRGBA(40, 50, 70, 255));
    nvgStrokeWidth(args.vg, 1.5f);
    nvgStroke(args.vg);

    if (!font) return;

    nvgFontFaceId(args.vg, font->handle);

    // Получаем указатель на модуль через родительский виджет
    ZeroMQSocketWidget* widget = dynamic_cast<ZeroMQSocketWidget*>(parent);
    ZeroMQSocket* module = widget ? dynamic_cast<ZeroMQSocket*>(widget->module) : nullptr;

    if (!module) {
        // Режим отображения в браузере модулей
        nvgFontSize(args.vg, 11.f);
        nvgFillColor(args.vg, nvgRGBA(100, 200, 255, 255));
        nvgText(args.vg, 10, 22, "ZMQ SOCKET", nullptr);
        nvgFillColor(args.vg, nvgRGBA(150, 150, 150, 255));
        nvgText(args.vg, 10, 42, "PORT: 5555", nullptr);
        nvgText(args.vg, 10, 58, "OFFLINE", nullptr);
        return;
    }

    // Вывод текущего порта сокета и протокола
    nvgFontSize(args.vg, 8.f);
    nvgFillColor(args.vg, nvgRGBA(100, 200, 255, 255));
    char portStr[32];
    std::snprintf(portStr, sizeof(portStr), "PORT: %d", module->currentActivePort);
    nvgText(args.vg, 8, 14, portStr, nullptr);

    char protoStr[32];
    std::snprintf(protoStr, sizeof(protoStr), "PROTO: %s", module->currentActiveProto == 1 ? "SUB" : "PULL");
    nvgText(args.vg, 8, 25, protoStr, nullptr);

    // Индикатор сетевой активности (Heartbeat-точка и статус)
    bool connected = (module->timeSinceLastPacket.load() < 1.0f);
    nvgBeginPath(args.vg);
    nvgCircle(args.vg, box.size.x - 12.f, 10.f, 3.f);
    if (connected) {
        nvgFillColor(args.vg, nvgRGBA(0, 255, 100, 255)); // Зелёный (активен)
    } else {
        nvgFillColor(args.vg, nvgRGBA(255, 50, 50, 255)); // Красный (нет пакетов)
    }
    nvgFill(args.vg);

    nvgFontSize(args.vg, 7.f);
    nvgFillColor(args.vg, nvgRGBA(150, 150, 150, 255));
    nvgText(args.vg, box.size.x - 68.f, 13, connected ? "ONLINE" : "OFFLINE", nullptr);

    // Вывод статуса байпаса
    bool bypassed = (module->params[ZeroMQSocket::BP_PARAM].getValue() > 0.5f);
    if (bypassed) {
        nvgFillColor(args.vg, nvgRGBA(255, 100, 100, 255));
        nvgText(args.vg, box.size.x - 68.f, 25, "BYPASS: ON", nullptr);
    } else {
        nvgFillColor(args.vg, nvgRGBA(100, 255, 150, 255));
        nvgText(args.vg, box.size.x - 68.f, 25, "BYPASS: OFF", nullptr);
    }

    // Секция мониторов CV (1-9) - маленькие шкалы уровня напряжения
    float barWidth = 8.f;
    float barSpacing = 4.f;
    float startX = 8.f;
    float startY = 44.f;
    float maxBarHeight = 10.f;

    nvgFontSize(args.vg, 6.f);
    nvgFillColor(args.vg, nvgRGBA(100, 110, 130, 255));
    nvgText(args.vg, startX, startY - 4.f, "CV INPUTS (1-9)", nullptr);

    for (int i = 0; i < 9; i++) {
        float val = bypassed ? 0.f : module->activeFrame.cv[i];
        if (val > 10.f) val = 10.f;
        if (val < -10.f) val = -10.f;

        // Приводим -10V..+10V к диапазону 0..1
        float norm = (val + 10.f) / 20.f;
        float barHeight = norm * maxBarHeight;
        float x = startX + i * (barWidth + barSpacing);

        // Фоновая подложка шкалы
        nvgBeginPath(args.vg);
        nvgRect(args.vg, x, startY, barWidth, maxBarHeight);
        nvgFillColor(args.vg, nvgRGBA(30, 35, 45, 255));
        nvgFill(args.vg);

        // Активный уровень сигнала
        nvgBeginPath(args.vg);
        nvgRect(args.vg, x, startY + (maxBarHeight - barHeight), barWidth, barHeight);
        nvgFillColor(args.vg, nvgRGBA(100, 200, 255, 200));
        nvgFill(args.vg);
    }

    // Секция монитора полифонических голосов (16 точек активности гейтов)
    float dotRadius = 1.8f;
    float dotSpacing = 6.6f;
    float pStartX = 9.f;
    float pStartY = 64.f;

    nvgFontSize(args.vg, 6.f);
    nvgFillColor(args.vg, nvgRGBA(100, 110, 130, 255));
    nvgText(args.vg, pStartX, pStartY - 5.f, "POLY VOICE ACTIVITY", nullptr);

    for (int c = 0; c < 16; c++) {
        bool active = !bypassed && (module->activeFrame.midi_gate[c] > 0.1f);
        float cx = pStartX + c * dotSpacing;
        float cy = pStartY + 1.f;

        nvgBeginPath(args.vg);
        nvgCircle(args.vg, cx, cy, dotRadius);
        if (active) {
            nvgFillColor(args.vg, nvgRGBA(255, 180, 0, 255)); // Янтарный (голос играет)
        } else {
            nvgFillColor(args.vg, nvgRGBA(40, 45, 55, 255)); // Выключен
        }
        nvgFill(args.vg);
    }
}

Model* modelZeroMQSocket = createModel<ZeroMQSocket, ZeroMQSocketWidget>("ZeroMQSocket");
