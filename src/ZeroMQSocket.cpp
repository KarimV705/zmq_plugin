#include "plugin.hpp"
#include <zmq.hpp>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstring>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <widget/TransformWidget.hpp>

// ─────────────────────────────────────────────────────────────
//  Структура пакета данных (57 float = 228 байт)
//  cv[9] + midi_pitch[16] + midi_gate[16] + midi_vel[16]
// ─────────────────────────────────────────────────────────────
struct ZmqAudioFrame {
    float cv[9];
    float midi_pitch[16];
    float midi_gate[16];
    float midi_vel[16];
};

// ─────────────────────────────────────────────────────────────
//  Вспомогательный snapshot — хранит копию данных для UI-потока
//  без захвата мьютекса при каждом draw().
// ─────────────────────────────────────────────────────────────
struct DisplaySnapshot {
    int    port        = 5555;
    int    proto       = 0;      // 0=PULL, 1=SUB
    bool   bypassed    = false;
    bool   connected   = false;
    float  cv[9]       = {};
    float  gate[16]    = {};
    float  pitch[16]   = {};
    int    theme       = 0;      // 0 = Light, 1 = Dark, 2 = Jungle, 3 = Vaporwave
};

// ─────────────────────────────────────────────────────────────
struct ZeroMQSocket : Module {
    enum ParamId  { PORT_PARAM, PROTO_PARAM, BP_PARAM, PARAMS_LEN };
    enum InputId  { INPUTS_LEN };
    enum OutputId {
        CV_OUT_1, CV_OUT_2, CV_OUT_3, CV_OUT_4, CV_OUT_5,
        CV_OUT_6, CV_OUT_7, CV_OUT_8, CV_OUT_9,
        POLY_PITCH_OUT, POLY_GATE_OUT, POLY_VEL_OUT,
        OUTPUTS_LEN
    };
    enum LightId { LIGHTS_LEN };

    // ── Сетевой поток ─────────────────────────────────────────
    std::thread*           workerThread    = nullptr;
    std::atomic<bool>      threadRunning   {false};

    // context и socket защищены workerMutex:
    // они создаются/уничтожаются только из аудио-потока через
    // requestRestart, а читаются только из воркера.
    std::mutex             workerMutex;
    zmq::context_t*        context         = nullptr;
    zmq::socket_t*         socket          = nullptr;

    // ── Двойной буфер (воркер → аудио) ───────────────────────
    std::mutex             bufferMutex;
    ZmqAudioFrame          backFrame       {};
    std::atomic<bool>      newFrameAvailable {false};

    // Аудио-буфер: читается ТОЛЬКО из process(), поэтому без блокировки.
    ZmqAudioFrame          activeFrame     {};

    // ── Snapshot для UI (аудио → draw) ────────────────────────
    // Атомарный флаг + простая структура, защищённая snapshotMutex.
    std::mutex             snapshotMutex;
    DisplaySnapshot        snapshot;
    std::atomic<bool>      snapshotDirty   {false};

    // ── Счётчик неактивности ──────────────────────────────────
    // FIX: используем int-счётчик вместо float-атомика, чтобы избежать
    // non-atomic read-modify-write (load+store — не атомарно).
    std::atomic<int>       inactiveFrames  {999999};

    // ── Состояние параметров ──────────────────────────────────
    int   currentActivePort  = 5555;
    int   currentActiveProto = 0;
    int   theme              = 0;  // 0 = Light (по умолчанию), 1 = Dark, 2 = Jungle, 3 = Vaporwave

    // FIX: флаг отложенного рестарта — изменение порта/протокола
    // запрашивается из process(), но выполняется асинхронно,
    // чтобы не блокировать аудио-поток во время join().
    std::atomic<bool>      restartRequested {false};
    std::thread            restartThread;

    // ─────────────────────────────────────────────────────────
    ZeroMQSocket() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

        // FIX: snapValue(1.f) даёт шаг 1 — целые числа портов
        configParam(PORT_PARAM, 1024.f, 65535.f, 5555.f, "ZMQ Port")->snapEnabled = true;
        configParam(PROTO_PARAM, 0.f, 1.f, 0.f, "Protocol (PULL/SUB)");
        configParam(BP_PARAM,   0.f, 1.f, 0.f, "Bypass");

        for (int i = 0; i < 9; i++)
            configOutput(CV_OUT_1 + i, "CV Channel " + std::to_string(i + 1));
        configOutput(POLY_PITCH_OUT, "Polyphonic Pitch (V/OCT)");
        configOutput(POLY_GATE_OUT,  "Polyphonic Gate");
        configOutput(POLY_VEL_OUT,   "Polyphonic Velocity");

        currentActivePort  = (int)std::round(params[PORT_PARAM].getValue());
        currentActiveProto = (int)params[PROTO_PARAM].getValue();
        startWorker(currentActivePort, currentActiveProto);
    }

    ~ZeroMQSocket() {
        // Убеждаемся, что поток рестарта завершён
        if (restartThread.joinable())
            restartThread.join();
        stopWorker();
    }

    // ── Запуск воркера ────────────────────────────────────────
    // Вызывается ТОЛЬКО из аудио-потока (или деструктора).
    void startWorker(int bindPort, int bindProto) {
        // FIX: полный сброс флагов перед запуском
        inactiveFrames.store(999999);
        newFrameAvailable.store(false);
        threadRunning.store(true);

        try {
            std::lock_guard<std::mutex> lk(workerMutex);
            context = new zmq::context_t(1);
            socket  = new zmq::socket_t(
                *context,
                bindProto == 1 ? zmq::socket_type::sub
                               : zmq::socket_type::pull
            );

            // FIX: заменяем сокет в лямбде на локальный указатель,
            // чтобы избежать обращения к уже удалённому указателю класса.
            zmq::socket_t* rawSocket = socket;

            workerThread = new std::thread([this, rawSocket, bindPort, bindProto]() {
                try {
                    std::string addr = "tcp://*:" + std::to_string(bindPort);
                    rawSocket->bind(addr);
                    rawSocket->set(zmq::sockopt::rcvtimeo, 200); // 200ms таймаут

                    if (bindProto == 1)
                        rawSocket->set(zmq::sockopt::subscribe, "");

                    zmq::message_t msg;
                    while (threadRunning.load(std::memory_order_relaxed)) {
                        // FIX: явный recv_flags::none, результат проверяем надёжно
                        auto res = rawSocket->recv(msg, zmq::recv_flags::none);

                        if (!res) {
                            // Таймаут (EAGAIN через rcvtimeo) — нормальная ситуация
                            continue;
                        }

                        if (msg.size() != sizeof(ZmqAudioFrame)) {
                            // FIX: пропускаем пакеты неверного размера вместо молчания
                            DEBUG("ZeroMQ: unexpected packet size %u (expected %u)",
                                  (unsigned)msg.size(), (unsigned)sizeof(ZmqAudioFrame));
                            continue;
                        }

                        {
                            std::lock_guard<std::mutex> lk(bufferMutex);
                            std::memcpy(&backFrame, msg.data(), sizeof(ZmqAudioFrame));
                        }
                        newFrameAvailable.store(true, std::memory_order_release);
                        inactiveFrames.store(0, std::memory_order_relaxed);
                    }
                } catch (const zmq::error_t& e) {
                    // ETERM — нормальное завершение при context->close()
                    if (e.num() != ETERM)
                        DEBUG("ZeroMQ worker error (port=%d, proto=%d): %s",
                              bindPort, bindProto, e.what());
                } catch (const std::exception& e) {
                    DEBUG("ZeroMQ worker std::exception: %s", e.what());
                }
                // FIX: воркер всегда сбрасывает флаг при выходе,
                // чтобы stopWorker() корректно определил завершение.
                threadRunning.store(false, std::memory_order_relaxed);
            });

        } catch (const std::exception& e) {
            // FIX: при ошибке инициализации освобождаем уже созданные объекты
            DEBUG("ZeroMQ init error: %s", e.what());
            threadRunning.store(false);
            std::lock_guard<std::mutex> lk(workerMutex);
            delete socket;  socket  = nullptr;
            delete context; context = nullptr;
        }
    }

    // ── Остановка воркера ─────────────────────────────────────
    // Вызывается из аудио-потока, деструктора или restartThread.
    void stopWorker() {
        threadRunning.store(false, std::memory_order_relaxed);

        // Закрываем сокет и контекст — это разблокирует recv() в воркере
        {
            std::lock_guard<std::mutex> lk(workerMutex);
            if (socket) {
                try { socket->close(); } catch (...) {}
            }
            if (context) {
                try { context->close(); } catch (...) {}
            }
        }

        if (workerThread) {
            if (workerThread->joinable())
                workerThread->join();
            delete workerThread;
            workerThread = nullptr;
        }

        {
            std::lock_guard<std::mutex> lk(workerMutex);
            delete socket;  socket  = nullptr;
            delete context; context = nullptr;
        }
    }

    // ── Асинхронный рестарт ───────────────────────────────────
    // FIX: вместо блокирующего stopWorker() в аудио-потоке —
    // запускаем рестарт в отдельном std::thread, аудио не тормозит.
    void scheduleRestart(int newPort, int newProto) {
        if (restartRequested.load()) return; // уже запланирован
        restartRequested.store(true);

        if (restartThread.joinable())
            restartThread.join();

        currentActivePort  = newPort;
        currentActiveProto = newProto;

        restartThread = std::thread([this, newPort, newProto]() {
            stopWorker();
            startWorker(newPort, newProto);
            restartRequested.store(false);
        });
    }

    // ─────────────────────────────────────────────────────────
    void process(const ProcessArgs& args) override {
        // ── Обнаружение изменения параметров ─────────────────
        // FIX: округляем значение knob до целого (snapEnabled не всегда
        // гарантирует int-значение в process).
        int targetPort  = (int)std::round(params[PORT_PARAM].getValue());
        int targetProto = (int)params[PROTO_PARAM].getValue();

        if (!restartRequested.load() &&
            (targetPort != currentActivePort || targetProto != currentActiveProto))
        {
            scheduleRestart(targetPort, targetProto);
        }

        // ── Счётчик неактивности ──────────────────────────────
        // FIX: инкрементируем int-атомик вместо float load+store.
        // Ограничиваем значение, чтобы не было переполнения при длительном
        // отсутствии данных (при 48kHz 999999 ≈ 20 секунд).
        int inactive = inactiveFrames.load(std::memory_order_relaxed);
        if (inactive < 999999)
            inactiveFrames.store(inactive + 1, std::memory_order_relaxed);

        // ── Получение нового фрейма от воркера ────────────────
        // FIX: проверяем acquire-флаг, затем берём мьютекс только если
        // данные действительно есть — минимизируем contention.
        if (newFrameAvailable.load(std::memory_order_acquire)) {
            if (bufferMutex.try_lock()) {
                activeFrame = backFrame;
                newFrameAvailable.store(false, std::memory_order_relaxed);
                bufferMutex.unlock();
            }
        }

        bool bypassed = (params[BP_PARAM].getValue() > 0.5f);

        // ── Вывод моно CV ─────────────────────────────────────
        for (int i = 0; i < 9; i++) {
            outputs[CV_OUT_1 + i].setVoltage(bypassed ? 0.f : activeFrame.cv[i]);
        }

        // ── Вывод полифонии ───────────────────────────────────
        // FIX: при bypass выставляем 0 каналов — downstream модули
        // (Split, VCO) должны видеть «нет сигнала», а не 16 каналов нулей.
        if (bypassed) {
            outputs[POLY_PITCH_OUT].setChannels(0);
            outputs[POLY_GATE_OUT].setChannels(0);
            outputs[POLY_VEL_OUT].setChannels(0);
        } else {
            // FIX: вычисляем реальное количество активных голосов
            // вместо всегда-16, чтобы не засорять полифонические кабели.
            int activeVoices = 0;
            for (int c = 15; c >= 0; c--) {
                if (activeFrame.midi_gate[c] > 0.001f) {
                    activeVoices = c + 1;
                    break;
                }
            }
            // Минимум 1 канал, даже если нет активных нот
            int numChannels = std::max(1, activeVoices);

            outputs[POLY_PITCH_OUT].setChannels(numChannels);
            outputs[POLY_GATE_OUT].setChannels(numChannels);
            outputs[POLY_VEL_OUT].setChannels(numChannels);

            for (int c = 0; c < numChannels; c++) {
                outputs[POLY_PITCH_OUT].setVoltage(activeFrame.midi_pitch[c], c);
                outputs[POLY_GATE_OUT].setVoltage(activeFrame.midi_gate[c], c);
                outputs[POLY_VEL_OUT].setVoltage(activeFrame.midi_vel[c], c);
            }
        }

        // ── Обновление snapshot для UI (раз в ~1ms, не каждый сэмпл) ─
        // FIX: вместо того чтобы UI-поток читал activeFrame напрямую
        // (data race), аудио-поток периодически публикует snapshot.
        static int uiUpdateCounter = 0;
        if (++uiUpdateCounter >= 48) { // ~1ms при 48kHz
            uiUpdateCounter = 0;
            bool connected = (inactiveFrames.load(std::memory_order_relaxed) < 48 * 1000); // <1 sec

            // try_lock чтобы не блокировать аудио если UI рисует
            if (snapshotMutex.try_lock()) {
                snapshot.port      = currentActivePort;
                snapshot.proto     = currentActiveProto;
                snapshot.bypassed  = bypassed;
                snapshot.connected = connected;
                snapshot.theme     = theme;  // Передаем тему в UI
                std::memcpy(snapshot.cv,    activeFrame.cv,         sizeof(snapshot.cv));
                std::memcpy(snapshot.gate,  activeFrame.midi_gate,  sizeof(snapshot.gate));
                std::memcpy(snapshot.pitch, activeFrame.midi_pitch, sizeof(snapshot.pitch));
                snapshotMutex.unlock();
                snapshotDirty.store(true, std::memory_order_release);
            }
        }
    }

    // ── Сериализация состояния ────────────────────────────────
    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "port",  json_integer(currentActivePort));
        json_object_set_new(root, "proto", json_integer(currentActiveProto));
        json_object_set_new(root, "theme", json_integer(theme));  // Сохраняем тему
        return root;
    }

    void dataFromJson(json_t* root) override {
        json_t* jPort  = json_object_get(root, "port");
        json_t* jProto = json_object_get(root, "proto");
        json_t* jTheme = json_object_get(root, "theme");  // Восстанавливаем тему
        if (jPort)  params[PORT_PARAM].setValue((float)json_integer_value(jPort));
        if (jProto) params[PROTO_PARAM].setValue((float)json_integer_value(jProto));
        if (jTheme) theme = json_integer_value(jTheme);
    }
};

// ─────────────────────────────────────────────────────────────
//  Forward declaration
// ─────────────────────────────────────────────────────────────
struct ZeroMQSocketWidget;

// ─────────────────────────────────────────────────────────────
//  OLED Дисплей
// ─────────────────────────────────────────────────────────────
struct OLEDDisplay : TransparentWidget {
    std::shared_ptr<Font> font;
    // FIX: кэш snapshot на стороне UI — никакого прямого обращения к Module
    DisplaySnapshot cachedSnapshot;

    OLEDDisplay() {
        box.size = Vec(120.f, 70.f);
        // FIX: загружаем шрифт из ресурсов плагина вместо системных ресурсов
        font = APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/ShareTechMono-Regular.ttf"));
    }

    void draw(const DrawArgs& args) override;
    void step() override;
};

// ─────────────────────────────────────────────────────────────
struct TransparentTransformWidget : widget::TransformWidget {
    float opacity = 0.4f;
    void draw(const DrawArgs& args) override {
        nvgSave(args.vg);
        nvgGlobalAlpha(args.vg, opacity);
        widget::TransformWidget::draw(args);
        nvgRestore(args.vg);
    }
    void drawLayer(const DrawArgs& args, int layer) override {
        nvgSave(args.vg);
        nvgGlobalAlpha(args.vg, opacity);
        widget::TransformWidget::drawLayer(args, layer);
        nvgRestore(args.vg);
    }
};

// ─────────────────────────────────────────────────────────────
struct ZeroMQSocketWidget : ModuleWidget {
    OLEDDisplay* display = nullptr;
    TransparentTransformWidget* logoTransform = nullptr;
    widget::SvgWidget* logoWidget = nullptr;
    std::shared_ptr<Font> panelFont;

    ZeroMQSocketWidget(ZeroMQSocket* module) {
        setModule(module);
        
        // Загружаем шрифт для отрисовки надписей панели в C++
        panelFont = APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/ShareTechMono-Regular.ttf"));

        // По умолчанию грузим светлую тему
        setPanel(createPanel(asset::plugin(pluginInstance, "res/ZeroMQSocket_light.svg")));

        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        display = createWidget<OLEDDisplay>(mm2px(Vec(5.08, 10.0)));
        addChild(display);

        // Добавляем логотип discotemple с масштабированием и прозрачностью через TransparentTransformWidget
        logoTransform = new TransparentTransformWidget();
        logoTransform->identity();
        
        // Масштабируем с 5334px (ширина оригинального SVG) до целевых 20мм (59.05px)
        float targetWidthPx = mm2px(20.0f);
        float scaleFactor = targetWidthPx / 5334.0f;
        logoTransform->scale(Vec(scaleFactor, scaleFactor));
        logoTransform->opacity = 0.4f; // 40% непрозрачности (полупрозрачный)
        
        logoWidget = new widget::SvgWidget();
        logoWidget->setSvg(APP->window->loadSvg(asset::plugin(pluginInstance, "res/discotemple_light.svg")));
        logoTransform->addChild(logoWidget);
        
        // Позиционируем в самом низу панели (по центру по горизонтали: x = 15.4мм, y = 120.0мм)
        logoTransform->box.pos = mm2px(Vec(15.4, 120.0));
        addChild(logoTransform);

        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.7, 47.0)), module, ZeroMQSocket::PORT_PARAM));
        addParam(createParamCentered<CKSS>(mm2px(Vec(25.4, 47.0)), module, ZeroMQSocket::PROTO_PARAM));
        addParam(createParamCentered<CKSS>(mm2px(Vec(38.1, 47.0)), module, ZeroMQSocket::BP_PARAM));

        for (int i = 0; i < 9; i++) {
            int row = i / 3, col = i % 3;
            float x = 12.7f + col * 12.7f;
            float y = 65.0f + row * 12.0f;
            addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x, y)), module, ZeroMQSocket::CV_OUT_1 + i));
        }

        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(12.7, 110.0)), module, ZeroMQSocket::POLY_PITCH_OUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(25.4, 110.0)), module, ZeroMQSocket::POLY_GATE_OUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(38.1, 110.0)), module, ZeroMQSocket::POLY_VEL_OUT));

        // Применяем тему (если загружен существующий патч)
        updateTheme();
    }

    void updateTheme() {
        ZeroMQSocket* module = dynamic_cast<ZeroMQSocket*>(this->module);
        if (!module) return;

        std::string svgPath;
        std::string logoPath;
        if (module->theme == 0) {
            svgPath = "res/ZeroMQSocket_light.svg";
            logoPath = "res/discotemple_light.svg";
        }
        else if (module->theme == 1) {
            svgPath = "res/ZeroMQSocket_dark.svg";
            logoPath = "res/discotemple_dark.svg";
        }
        else if (module->theme == 2) {
            svgPath = "res/ZeroMQSocket_jungle.svg";
            logoPath = "res/discotemple_jungle.svg";
        }
        else if (module->theme == 3) {
            svgPath = "res/ZeroMQSocket_vaporwave.svg";
            logoPath = "res/discotemple_vaporwave.svg";
        }

        // Используем метод getPanel() вместо прямого доступа к panel
        app::SvgPanel* svgPanel = dynamic_cast<app::SvgPanel*>(getPanel());
        if (svgPanel) {
            svgPanel->setBackground(APP->window->loadSvg(asset::plugin(pluginInstance, svgPath)));
        }

        if (logoWidget) {
            logoWidget->setSvg(APP->window->loadSvg(asset::plugin(pluginInstance, logoPath)));
        }
    }

    // Отрисовываем надписи на панели динамически
    void draw(const DrawArgs& args) override {
        // Сначала рисуем саму панель (SVG фоны без текста)
        ModuleWidget::draw(args);

        ZeroMQSocket* module = dynamic_cast<ZeroMQSocket*>(this->module);
        int currentTheme = module ? module->theme : 0; // По умолчанию Light (0) для браузера модулей

        // Выбираем цвета надписей в зависимости от темы
        NVGcolor textColor;
        NVGcolor subColor;

        if (currentTheme == 0) { // Light
            textColor = nvgRGBA(40, 40, 40, 255);
            subColor = nvgRGBA(96, 96, 96, 255);
        } else if (currentTheme == 1) { // Dark
            textColor = nvgRGBA(160, 161, 165, 255);
            subColor = nvgRGBA(96, 97, 101, 255);
        } else if (currentTheme == 2) { // Jungle
            textColor = nvgRGBA(57, 255, 20, 255); // Neon lime green
            subColor = nvgRGBA(255, 215, 0, 255); // Yellow/banana
        } else { // Vaporwave
            textColor = nvgRGBA(1, 205, 254, 255); // Neon cyan
            subColor = nvgRGBA(255, 113, 206, 255); // Synthwave pink
        }

        if (!panelFont) return;
        nvgFontFaceId(args.vg, panelFont->handle);
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);

        // 1. Заголовки ручек управления (y = 116)
        nvgFontSize(args.vg, 9.f);
        nvgFillColor(args.vg, textColor);
        nvgText(args.vg, 37.5f, 116.f, "PORT", nullptr);
        nvgText(args.vg, 75.f, 116.f, "PROTO", nullptr);
        nvgText(args.vg, 112.5f, 116.f, "BP", nullptr);

        // 2. Секция CV Out
        nvgFontSize(args.vg, 9.f);
        nvgFillColor(args.vg, textColor);
        nvgText(args.vg, 75.f, 172.f, "CV OUT (1-9)", nullptr);

        nvgFontSize(args.vg, 8.5f);
        nvgFillColor(args.vg, subColor);
        // CV Numbers (Сетка выходов - смещены вниз под гнезда: 204px, 239px, 274px)
        nvgText(args.vg, 37.5f, 204.f, "1", nullptr);
        nvgText(args.vg, 75.f, 204.f, "2", nullptr);
        nvgText(args.vg, 112.5f, 204.f, "3", nullptr);

        nvgText(args.vg, 37.5f, 239.f, "4", nullptr);
        nvgText(args.vg, 75.f, 239.f, "5", nullptr);
        nvgText(args.vg, 112.5f, 239.f, "6", nullptr);

        nvgText(args.vg, 37.5f, 274.f, "7", nullptr);
        nvgText(args.vg, 75.f, 274.f, "8", nullptr);
        nvgText(args.vg, 112.5f, 274.f, "9", nullptr);

        // 3. Секция Poly MIDI Out
        nvgFontSize(args.vg, 9.f);
        nvgFillColor(args.vg, textColor);
        nvgText(args.vg, 75.f, 302.f, "POLY MIDI OUT", nullptr);

        nvgFontSize(args.vg, 8.f);
        nvgFillColor(args.vg, subColor);
        nvgText(args.vg, 37.5f, 344.f, "PITCH", nullptr);
        nvgText(args.vg, 75.f, 344.f, "GATE", nullptr);
        nvgText(args.vg, 112.5f, 344.f, "VEL", nullptr);
    }

    // Добавляем меню выбора темы при правом клике
    void appendContextMenu(Menu* menu) override {
        ModuleWidget::appendContextMenu(menu);

        ZeroMQSocket* module = dynamic_cast<ZeroMQSocket*>(this->module);
        if (!module) return;

        menu->addChild(new MenuSeparator());

        MenuLabel* themeLabel = new MenuLabel();
        themeLabel->text = "Theme";
        menu->addChild(themeLabel);

        struct ThemeMenuItem : MenuItem {
            ZeroMQSocketWidget* widget;
            int themeId;
            void onAction(const event::Action& e) override {
                ZeroMQSocket* m = widget->getModule<ZeroMQSocket>();
                if (m) {
                    m->theme = themeId;
                }
                widget->updateTheme();
            }
        };

        const std::vector<std::string> themeNames = {"Light", "Dark", "Jungle", "Vaporwave"};
        for (size_t i = 0; i < themeNames.size(); i++) {
            ThemeMenuItem* item = new ThemeMenuItem();
            item->text = themeNames[i];
            item->widget = this;
            item->themeId = i;
            item->rightText = (module->theme == (int)i) ? "✔" : "";
            menu->addChild(item);
        }
    }
};

// ─────────────────────────────────────────────────────────────
//  OLEDDisplay::step() — вызывается из UI-потока каждый кадр.
// ─────────────────────────────────────────────────────────────
void OLEDDisplay::step() {
    TransparentWidget::step();

    ZeroMQSocketWidget* widget = dynamic_cast<ZeroMQSocketWidget*>(parent);
    if (!widget) return;
    ZeroMQSocket* module = dynamic_cast<ZeroMQSocket*>(widget->module);
    if (!module) return;

    // Забираем snapshot только если он обновился
    if (module->snapshotDirty.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lk(module->snapshotMutex);
        cachedSnapshot = module->snapshot;
        module->snapshotDirty.store(false, std::memory_order_relaxed);
    }
}

// ─────────────────────────────────────────────────────────────
static std::string getNoteName(float voltage) {
    int noteNum = (int)std::round(voltage * 12.f + 60.f);
    if (noteNum < 0 || noteNum > 127) return "--";
    const char* noteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    std::string name = noteNames[noteNum % 12];
    int octave = (noteNum / 12) - 1;
    return name + std::to_string(octave);
}

// ─────────────────────────────────────────────────────────────
//  OLEDDisplay::draw() — читает только cachedSnapshot.
// ─────────────────────────────────────────────────────────────
void OLEDDisplay::draw(const DrawArgs& args) {
    // ── Режим браузера (нет модуля) ───────────────────────────
    ZeroMQSocketWidget* widget = dynamic_cast<ZeroMQSocketWidget*>(parent);
    bool hasModule = widget && widget->module;
    if (!hasModule) {
        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 4.f);
        nvgFillColor(args.vg, nvgRGBA(10, 13, 20, 255));
        nvgFill(args.vg);

        nvgStrokeColor(args.vg, nvgRGBA(40, 50, 70, 255));
        nvgStrokeWidth(args.vg, 1.5f);
        nvgStroke(args.vg);

        if (!font) return;
        nvgFontFaceId(args.vg, font->handle);

        nvgFontSize(args.vg, 11.f);
        nvgFillColor(args.vg, nvgRGBA(100, 200, 255, 255));
        nvgText(args.vg, 10, 22, "ZMQ SOCKET", nullptr);
        nvgFillColor(args.vg, nvgRGBA(150, 150, 150, 255));
        nvgText(args.vg, 10, 42, "PORT: 5555", nullptr);
        nvgText(args.vg, 10, 58, "OFFLINE", nullptr);
        return;
    }

    const DisplaySnapshot& s = cachedSnapshot;

    // Схемы цветов и текстовые метки для тем
    NVGcolor bgColor;
    NVGcolor borderColor;
    NVGcolor textColor;
    NVGcolor textDimColor;
    NVGcolor accentColor;

    std::string offlineText = "OFFLINE";
    std::string onlineText = "ONLINE";
    std::string bypassOnText = "BYPASS: ON";
    std::string bypassOffText = "BYPASS: OFF";

    if (s.theme == 0) { // Light Theme
        bgColor = nvgRGBA(238, 238, 238, 255);
        borderColor = nvgRGBA(176, 176, 176, 255);
        textColor = nvgRGBA(32, 32, 32, 255);
        textDimColor = nvgRGBA(112, 112, 112, 255);
        accentColor = s.connected ? nvgRGBA(0, 160, 64, 255) : nvgRGBA(208, 32, 32, 255);
    } else if (s.theme == 1) { // Dark Theme
        bgColor = nvgRGBA(10, 13, 20, 255);
        borderColor = nvgRGBA(40, 50, 70, 255);
        textColor = nvgRGBA(100, 200, 255, 255);
        textDimColor = nvgRGBA(150, 150, 150, 255);
        accentColor = s.connected ? nvgRGBA(0, 255, 100, 255) : nvgRGBA(255, 50, 50, 255);
    } else if (s.theme == 2) { // Jungle Theme
        bgColor = nvgRGBA(8, 20, 9, 255);
        borderColor = nvgRGBA(85, 107, 47, 255);
        textColor = nvgRGBA(57, 255, 20, 255);
        textDimColor = nvgRGBA(107, 142, 35, 255);
        accentColor = s.connected ? nvgRGBA(255, 215, 0, 255) : nvgRGBA(255, 69, 0, 255);

        offlineText = "LOST IN JUNGLE 🌴";
        onlineText = "HUNTING 🐆";
        bypassOnText = "RESTING 💤";
        bypassOffText = "SLASHING 🪵";
    } else { // Vaporwave Theme
        bgColor = nvgRGBA(43, 15, 84, 255);
        borderColor = nvgRGBA(255, 0, 127, 255);
        textColor = nvgRGBA(1, 205, 254, 255);
        textDimColor = nvgRGBA(185, 103, 255, 255);
        accentColor = s.connected ? nvgRGBA(255, 251, 150, 255) : nvgRGBA(5, 0, 255, 255);

        offlineText = "S A D N E S S 💾";
        onlineText = "V I B I N G 🌌";
        bypassOnText = "S A D B O Y 😭";
        bypassOffText = "R U N N I N G 🚗";
    }

    // Отрисовываем фон с выбранным цветом
    nvgBeginPath(args.vg);
    nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 4.f);
    nvgFillColor(args.vg, bgColor);
    nvgFill(args.vg);

    // Рамка с выбранным цветом
    nvgStrokeColor(args.vg, borderColor);
    nvgStrokeWidth(args.vg, 1.5f);
    nvgStroke(args.vg);

    if (!font) return;
    nvgFontFaceId(args.vg, font->handle);

    // ── Порт и протокол ───────────────────────────────────────
    char buf[64];
    nvgFontSize(args.vg, 8.f);
    nvgFillColor(args.vg, textColor);
    nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
    std::snprintf(buf, sizeof(buf), "PORT: %d", s.port);
    nvgText(args.vg, 8.f, 13.f, buf, nullptr);

    std::snprintf(buf, sizeof(buf), "PROTO: %s", s.proto == 1 ? "SUB" : "PULL");
    nvgText(args.vg, 8.f, 23.f, buf, nullptr);

    // ── Heartbeat-точка и статус ──────────────────────────────
    nvgBeginPath(args.vg);
    nvgCircle(args.vg, box.size.x - 12.f, 9.f, 3.f);
    nvgFillColor(args.vg, accentColor);
    nvgFill(args.vg);

    nvgFontSize(args.vg, 7.f);
    nvgFillColor(args.vg, textDimColor);
    nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BASELINE);
    nvgText(args.vg, box.size.x - 20.f, 12.f, s.connected ? onlineText.c_str() : offlineText.c_str(), nullptr);

    // ── Статус байпаса ────────────────────────────────────────
    if (s.bypassed) {
        nvgFillColor(args.vg, nvgRGBA(255, 100, 100, 255));
        nvgText(args.vg, box.size.x - 20.f, 23.f, bypassOnText.c_str(), nullptr);
    } else {
        nvgFillColor(args.vg, s.theme == 0 ? nvgRGBA(0, 150, 50, 255) : nvgRGBA(100, 255, 150, 255));
        nvgText(args.vg, box.size.x - 20.f, 23.f, bypassOffText.c_str(), nullptr);
    }

    // ── Сетка CV монитора (3x3 числовые значения) ────────────────
    nvgFontSize(args.vg, 6.2f);
    nvgFillColor(args.vg, textDimColor);
    
    for (int i = 0; i < 9; i++) {
        int col = i % 3;
        int row = i / 3;
        float x = 8.f + col * 38.f;
        float y = 35.f + row * 8.f;
        
        char cvBuf[32];
        float val = s.bypassed ? 0.f : s.cv[i];
        std::snprintf(cvBuf, sizeof(cvBuf), "CV%d:%+.1fV", i + 1, val);
        
        nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
        nvgText(args.vg, x, y, cvBuf, nullptr);
    }

    // ── Разделительная линия ──────────────────────────────────
    nvgBeginPath(args.vg);
    nvgMoveTo(args.vg, 6.f, 57.f);
    nvgLineTo(args.vg, box.size.x - 6.f, 57.f);
    nvgStrokeColor(args.vg, s.theme == 0 ? nvgRGBA(200, 200, 200, 255) : nvgRGBA(50, 60, 80, 255));
    nvgStrokeWidth(args.vg, 0.5f);
    nvgStroke(args.vg);

    // ── Монитор активных нот полифонии ────────────────────────
    int activeCount = 0;
    std::vector<std::string> activeNotes;
    for (int c = 0; c < 16; c++) {
        if (!s.bypassed && s.gate[c] > 0.1f) {
            activeCount++;
            if (activeNotes.size() < 5) {
                activeNotes.push_back(getNoteName(s.pitch[c]));
            }
        }
    }

    std::string notesStr = "";
    for (size_t i = 0; i < activeNotes.size(); i++) {
        if (i > 0) notesStr += " ";
        notesStr += activeNotes[i];
    }
    if (activeCount > 5) {
        notesStr += "...";
    }

    nvgFontSize(args.vg, 6.8f);
    nvgFillColor(args.vg, textColor);
    
    char voicesBuf[128];
    if (activeCount > 0) {
        std::snprintf(voicesBuf, sizeof(voicesBuf), "VOICES: %d [%s]", activeCount, notesStr.c_str());
    } else {
        std::snprintf(voicesBuf, sizeof(voicesBuf), "VOICES: 0 [-]");
    }
    
    nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
    nvgText(args.vg, 8.f, 65.f, voicesBuf, nullptr);
}

Model* modelZeroMQSocket = createModel<ZeroMQSocket, ZeroMQSocketWidget>("ZeroMQSocket");
