#include "power_mgmt.h"

// =============================================================================
// power_mgmt.cpp —— 电源管理模块（底层）实现
//
// 【本文件的作用】
// 实现按键中断与消抖、LED 指示、电源状态机、省电模式、长按关机进深睡。
// 这些代码原先是 src/app.cpp 的一部分（原 :103-287），按分层重构搬迁至此，
// 逻辑逐行保持不变，仅把两处「向上查询」改为「由应用层传入」：
//   - 原 updateLED() 里直接调 recorder_sd_ok() → 改为读 s_sdMounted 静态量
//   - 原 updateLED()/shutdownDevice() 里直接读全局 connected → 改为读 s_bleConnected
//
// 【分层】底层。不 include 任何业务模块或协议层的头文件。
// 【配套头文件】power_mgmt.h（对外接口说明）
// =============================================================================

#include "config.h" // 引脚号（POWER_BUTTON_PIN / STATUS_LED_PIN）、时间阈值、led_status_t 等
#include "esp_sleep.h"
#include "mic.h" // shutdownDevice() 里要调 mic_stop() 停掉麦克风

// -------------------------------------------------------------------------
// 文件内状态量（原为 app.cpp 的全局量，现收进本模块，均为文件静态）
// 定义位置：本文件；原来的定义位置是 src/app.cpp:30-36
// -------------------------------------------------------------------------
static volatile bool buttonPressed = false; // 按键 ISR 置位标志（原 app.cpp:30）
static unsigned long buttonPressTime = 0;   // 按键按下的时刻，用于算长按（原 app.cpp:31）
static led_status_t ledMode = LED_BOOT_SEQUENCE; // 当前 LED 指示模式（原 app.cpp:32）
static unsigned long lastActivity = 0;      // 最近一次「用户活动」时刻（原 app.cpp:35）
static bool powerSaveMode = false;          // 是否处于省电模式（原 app.cpp:36）

// 由 power_mgmt_poll() 每轮写入，供内部函数读取；替代原先对全局量/跨模块函数的直接访问
static bool s_bleConnected = false; // BLE 是否已连接（原来直接读 app.cpp 的 connected）
static bool s_sdMounted = false;    // SD 卡是否已挂载（原来直接调 recorder_sd_ok()）

// -------------------------------------------------------------------------
// 前向声明（与重构前 app.cpp:87-93 的声明块一致：这些函数在本文件内互相引用）
// -------------------------------------------------------------------------
void IRAM_ATTR buttonISR(); // 按键中断服务程序
void handleButton();        // 按键轮询处理
void updateLED();           // LED 刷新
void blinkLED(int count, int delayMs); // 阻塞式闪烁（当前无调用点，保留）
void enterPowerSave();      // 进入省电
void exitPowerSave();       // 退出省电
void shutdownDevice();      // 关机进深睡

// -------------------------------------------------------------------------
// Button ISR
// -------------------------------------------------------------------------
/**
 * @brief 按键电平变化中断服务程序（IRAM，必须极短）。
 * 功能：仅置位 buttonPressed 标志，实际处理交给 handleButton() 在主循环做。
 * 入参/出参：无（中断无参、无返回）。
 * 引用的变量：buttonPressed（定义在本文件上方）。
 */
void IRAM_ATTR buttonISR()
{
    buttonPressed = true; // 只置标志，不在中断里做任何耗时操作
}

// -------------------------------------------------------------------------
// LED Functions
// -------------------------------------------------------------------------
/**
 * @brief 按当前 ledMode 刷新 LED 指示。
 * 功能：开机快闪 → 正常指示（连接常亮/未连接慢闪）；关机时快闪两下后进深睡。
 *       LED 为反相逻辑（HIGH=灭，LOW=亮）。
 * 入参/出参：无。
 * 引用的变量：
 *   - ledMode / s_bleConnected / s_sdMounted → 定义在本文件上方
 *   - STATUS_LED_PIN / LED_* 各种时长 → src/config.h
 *   - shutdownDevice() → 定义在本文件下方
 */
void updateLED()
{
#if VOICEPRINT_ENABLE
    // GPIO21 与扩展板 SD 片选共用。启用录音模块后，该引脚归 SD 独占，
    // 固件不再驱动 LED。
    //
    // 关键教训：早期版本只在「SD 已挂载」时才停止驱动 GPIO21。结果在
    // 恰好「未有卡/挂载失败」的情况下，本函数会把 CS 拉低、并每 1 秒翻转一次，
    // SD 卡收不到合法的复位时序（CMD0 失败），于是永远挂不上卡、也无法重试。
    // 代价：本构建没有状态 LED。
    return; // VOICEPRINT_ENABLE 构建下直接返回：GPIO21 让给 SD，不再驱动 LED
#endif

    // GPIO21 doubles as the expansion board's SD chip select. Once the card is
    // mounted, driving this pin would corrupt SD transactions, so the status LED
    // goes dark for good - the accepted cost of using the SD slot.
    if (s_sdMounted) { // 原来这里是 if (recorder_sd_ok())，改为读应用层传入的静态量
        return;
    }

    unsigned long now = millis(); // 本轮时间基准
    static unsigned long bootStartTime = 0;   // 开机闪烁序列的起始时刻
    static unsigned long powerOffStartTime = 0; // 关机闪烁序列的起始时刻

    switch (ledMode) {
    case LED_BOOT_SEQUENCE:
        if (bootStartTime == 0)
            bootStartTime = now; // 首次进入时记录起始时刻

        // 5 quick blinks over 1.5 seconds total (inverted logic: HIGH=OFF, LOW=ON)
        if (now - bootStartTime < 1500) {
            int blinkPhase = ((now - bootStartTime) / 150) % 2; // 每 150ms 翻转一次，共 5 个亮灭周期
            digitalWrite(STATUS_LED_PIN, !blinkPhase);          // 反相输出
        } else {
            digitalWrite(STATUS_LED_PIN, HIGH); // OFF 开机序列结束，先熄灭
            ledMode = LED_NORMAL_OPERATION;     // 转入正常指示模式
            bootStartTime = 0;                  // 复位，便于下次重新计时
        }
        break;

    case LED_POWER_OFF_SEQUENCE:
        if (powerOffStartTime == 0)
            powerOffStartTime = now; // 首次进入时记录起始时刻

        // 2 quick blinks over 800ms total (inverted logic: HIGH=OFF, LOW=ON)
        if (now - powerOffStartTime < 800) {
            int blinkPhase = ((now - powerOffStartTime) / 200) % 2; // 每 200ms 翻转一次
            digitalWrite(STATUS_LED_PIN, !blinkPhase);              // 反相输出
        } else {
            digitalWrite(STATUS_LED_PIN, HIGH); // OFF 关机序列结束
            delay(100);                         // 等 LED 稳定后再断电
            shutdownDevice();                   // 进入深睡（不返回）
        }
        break;

    case LED_NORMAL_OPERATION:
    default:
        if (s_bleConnected) { // 原来读全局 connected，现读应用层传入的静态量
            // Connected - LED solid ON
            digitalWrite(STATUS_LED_PIN, LOW); // 常亮
        } else {
            // Disconnected - LED slow blink (1 sec on, 1 sec off)
            int blinkPhase = (now / 1000) % 2;               // 每 1 秒翻转一次
            digitalWrite(STATUS_LED_PIN, blinkPhase ? HIGH : LOW); // 慢闪
        }
        break;
    }
}

/**
 * @brief 阻塞式闪烁 LED 若干次。
 * 功能：亮/灭各持续 delayMs 毫秒，重复 count 次。注意本函数内含 delay，会阻塞调用者。
 * 入参：
 *   - count  ：闪烁次数。
 *   - delayMs：每次亮/灭各持续的毫秒数。
 * 出参：无。
 * 引用的变量：STATUS_LED_PIN（src/config.h）。
 * 备注：当前构建中无任何调用点（既有状态），按「用户没让改的一律不改」原则原样保留。
 */
void blinkLED(int count, int delayMs)
{
    for (int i = 0; i < count; i++) {        // 循环 count 次
        digitalWrite(STATUS_LED_PIN, HIGH);  // 灭（反相逻辑）
        delay(delayMs);                      // 保持
        digitalWrite(STATUS_LED_PIN, LOW);   // 亮
        delay(delayMs);                      // 保持
    }
}

// -------------------------------------------------------------------------
// Button Handling
// -------------------------------------------------------------------------
/**
 * @brief 按键轮询处理：消抖、短按/长按判定。
 * 功能：短按记录活动（并退出省电）；长按 ≥2000ms 触发关机序列。
 * 入参/出参：无。
 * 引用的变量：
 *   - buttonPressed / buttonPressTime / ledMode / lastActivity → 定义在本文件上方
 *   - powerSaveMode → 定义在本文件上方；exitPowerSave() → 本文件下方
 *   - POWER_BUTTON_PIN / BUTTON_DEBOUNCE_MS / POWER_OFF_PRESS_MS → src/config.h
 */
void handleButton()
{
    unsigned long now = millis();       // 本轮时间基准（注意：与 loop 顶部那个 now 不是同一个）
    static unsigned long lastDebounceTime = 0; // 上次状态跳变时刻，用于消抖
    static bool buttonDown = false;            // 当前是否处于「按下」状态
    static bool longPressTriggered = false;    // 本次按下是否已触发过长按

    bool currentButtonState = !digitalRead(POWER_BUTTON_PIN); // Active low (pressed = true) 低电平有效

    if (currentButtonState && !buttonDown) {
        // Button just pressed - debounce
        if (now - lastDebounceTime < 50) { // 50ms 内的抖动忽略
            return;
        }
        buttonPressTime = now;      // 记录按下时刻，供长按判定
        buttonDown = true;          // 置位按下状态
        longPressTriggered = false; // 新一轮按下，清除长按标志
        lastDebounceTime = now;     // 更新消抖基准

    } else if (currentButtonState && buttonDown && !longPressTriggered) {
        // Button still held - check for long press
        unsigned long pressDuration = now - buttonPressTime; // 已按住多久
        if (pressDuration >= 2000) {
            // Long press threshold reached - trigger power off immediately
            longPressTriggered = true;          // 标记已触发，避免重复
            ledMode = LED_POWER_OFF_SEQUENCE;   // 切到关机闪烁序列（由 updateLED 执行）
        }

    } else if (!currentButtonState && buttonDown) {
        // Button just released - debounce
        if (now - lastDebounceTime < 50) { // 释放抖动同样忽略
            return;
        }
        buttonDown = false;                                 // 清除按下状态
        unsigned long pressDuration = now - buttonPressTime; // 本次按住总时长
        lastDebounceTime = now;                              // 更新消抖基准

        // Only handle short press if long press wasn't already triggered
        if (!longPressTriggered && pressDuration >= 50) { // 有效短按（≥50ms）
            // Short press - register activity
            lastActivity = now;    // 记录用户活动，推迟省电
            if (powerSaveMode) {
                exitPowerSave();   // 若在省电模式，立即退出
            }
        }
        longPressTriggered = false; // 收尾复位
    }

    buttonPressed = false; // 清中断标志（本函数以轮询为主，标志仅作记录）
}

// -------------------------------------------------------------------------
// Power Management
// -------------------------------------------------------------------------
/**
 * @brief 进入省电模式：把 CPU 降到最低频。
 * 入参/出参：无。
 * 引用的变量：powerSaveMode（本文件上方）；MIN_CPU_FREQ_MHZ（src/config.h）。
 */
void enterPowerSave()
{
    if (!powerSaveMode) {                       // 幂等：已在省电则不重复降频
        setCpuFrequencyMhz(MIN_CPU_FREQ_MHZ);   // 40MHz for idle
        powerSaveMode = true;                   // 置位状态
    }
}

/**
 * @brief 退出省电模式：把 CPU 恢复到正常频率。
 * 入参/出参：无。
 * 引用的变量：powerSaveMode（本文件上方）；NORMAL_CPU_FREQ_MHZ（src/config.h）。
 */
void exitPowerSave()
{
    if (powerSaveMode) {                            // 仅在省电模式下才需要恢复
        setCpuFrequencyMhz(NORMAL_CPU_FREQ_MHZ);    // Back to 80MHz
        powerSaveMode = false;                      // 清除状态
    }
}

/**
 * @brief 关机：停音频 → 配置按键唤醒 → 进入深睡（本函数不返回）。
 * 入参/出参：无。
 * 引用的变量：
 *   - s_bleConnected（本文件上方，原来读全局 connected）
 *   - STATUS_LED_PIN（src/config.h）；mic_stop()（src/mic.h）
 */
void shutdownDevice()
{
    Serial.println("Shutting down device..."); // 关机日志

    // Stop audio
    mic_stop(); // 先停麦克风，避免深睡前还在采数据

    // Disconnect BLE gracefully
    if (s_bleConnected) { // 原来读全局 connected；现读应用层传入的静态量
        Serial.println("Disconnecting BLE...");
    }

    // Turn off LED (inverted logic)
    digitalWrite(STATUS_LED_PIN, HIGH); // 熄灯（反相逻辑 HIGH=灭）

    // Enter deep sleep
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_1, 0); // Wake on button press 按键（GPIO1）低电平唤醒
    Serial.println("Entering deep sleep...");    // 日志
    delay(100);                                  // 让串口日志发完
    esp_deep_sleep_start();                      // 进入深睡（不返回）
}

// =========================================================================
// 对外接口（实现说明见 power_mgmt.h）
// =========================================================================

/**
 * @brief 初始化电源管理相关的一切（GPIO / 中断 / LED 初值 / CPU 频率 / 活动计时）。
 * 入参/出参：无。
 * 说明：逐行等价于原 setup_app() 的 app.cpp:1088-1098 与 :1101-1102。
 */
void power_mgmt_init(void)
{
    pinMode(POWER_BUTTON_PIN, INPUT_PULLUP); // 按键：上拉输入（按下=低）
    pinMode(STATUS_LED_PIN, OUTPUT);         // LED：输出

    // LED uses inverted logic: HIGH = OFF, LOW = ON
    digitalWrite(STATUS_LED_PIN, HIGH); // 初始熄灭

    // Setup button interrupt
    attachInterrupt(digitalPinToInterrupt(POWER_BUTTON_PIN), buttonISR, CHANGE); // 电平变化触发

    // Start LED boot sequence
    ledMode = LED_BOOT_SEQUENCE; // 进入开机闪烁序列

    // Power optimization from config.h
    setCpuFrequencyMhz(NORMAL_CPU_FREQ_MHZ); // 设定正常档 CPU 频率
    lastActivity = millis();                 // 开机即算一次活动，避免刚开机就省电
}

/**
 * @brief 每轮主循环：处理按键 + 刷新 LED。
 * 入参：bleConnected（BLE 是否已连接）、sdMounted（SD 卡是否已挂载）。
 * 出参：无。
 * 说明：等价于原 loop_app() 里先后调用的 handleButton()(:1160) 与 updateLED()(:1163)。
 */
void power_mgmt_poll(bool bleConnected, bool sdMounted)
{
    s_bleConnected = bleConnected; // 缓存供 updateLED() / shutdownDevice() 读取
    s_sdMounted = sdMounted;       // 缓存供 updateLED() 读取

    handleButton(); // 原 loop_app():1160
    updateLED();    // 原 loop_app():1163
}

/**
 * @brief 每轮主循环：省电模式进入/退出判断。
 * 入参：now（循环顶部捕获的 millis()，由 app.cpp 传入）、bleConnected（BLE 是否已连接）。
 * 出参：无。
 * 说明：逐行等价于原 loop_app() 的 app.cpp:1189-1195。
 *
 * ⚠️ 为什么 now 必须由调用方传入：
 * 原代码在 loop 顶部捕获 now(:1157)，随后 handleButton() 可能在短按时把 lastActivity
 * 更新为**更晚**的 millis()(:236)。于是这里算出的 now - lastActivity 是一个负数，
 * 在 unsigned long 下下溢成极大值，必然大于 IDLE_THRESHOLD_MS → 短按后立刻进入省电。
 * 这是一个**既存缺陷**，但本次是纯搬迁重构，必须逐位复现、不得顺手修好。
 * 因此 now 一律由 app.cpp 传入同一个值，本函数绝不自己重新取 millis()。
 */
void power_mgmt_idle_check(unsigned long now, bool bleConnected)
{
    if (!bleConnected && (now - lastActivity > IDLE_THRESHOLD_MS)) { // 未连接且长时间无活动
        enterPowerSave(); // 降频省电
    } else if (bleConnected) { // 已连接：始终保持全速
        if (powerSaveMode)
            exitPowerSave(); // 若在省电则恢复频率
        lastActivity = now;  // 连接期间持续刷新活动时间
    }
}

/**
 * @brief 记录一次用户活动（用于推迟省电）。
 * 入参/出参：无。
 * 说明：等价于原 ServerHandler::onConnect() 的 app.cpp:818。
 */
void power_mgmt_note_activity(void)
{
    lastActivity = millis(); // 刷新活动时间戳
}
