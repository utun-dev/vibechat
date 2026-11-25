#include "../src/ptt_key_chord.h"
#include "../src/ptt_hotkey_state.h"
#include <cstdio>

static int failures;
#define CHECK(value) do { if (!(value)) { std::printf("FAIL line %d: %s\n", __LINE__, #value); failures++; } } while (0)
int main() {
    // Физические CtrlL/WinL и ShiftL/ShiftR на Windows; алгоритм не зависит от платформы.
    PttKeyChord ctrlWin{162, 91}, shifts{160, 161};
    CHECK(!pttChordHeld({}, {}));
    CHECK(!pttChordHeld(ctrlWin, {162}));
    CHECK(pttChordHeld(ctrlWin, {162, 91}));
    CHECK(pttChordHeld(ctrlWin, {162, 91, 65}));
    CHECK(!pttChordHeld(ctrlWin, {91})); // отпускание любого участника прекращает передачу
    CHECK(!pttChordHeld(shifts, {160}));
    CHECK(!pttChordHeld(shifts, {161}));
    CHECK(pttChordHeld(shifts, {160, 161}));
    CHECK(!pttChordHeld(ctrlWin, {163, 92})); // правые клавиши не подменяют левые
    CHECK(pttChordHeld({65, 66}, {65, 66})); // две обычные клавиши
    CHECK(pttDecodeChord(pttEncodeChord(shifts)) == shifts);
    CHECK(pttEncodeChord(ctrlWin) == "91,162");
    CHECK(pttDecodeChord("0,162").isEmpty());
    CHECK(pttDecodeChord("256").isEmpty());
    CHECK(pttDecodeChord("Shift+F8").isEmpty());
    CHECK(pttDecodeChord("91,invalid").isEmpty());
    CHECK(pttDecodeChord("91,162,91") == ctrlWin);
    PttChordCapture capture;
    CHECK(!capture.observe({}));
    CHECK(!capture.observe({160}));
    CHECK(!capture.observe(shifts));
    CHECK(!capture.observe({161})); // отпускание первой клавиши сохраняет полную комбинацию
    CHECK(capture.observe({}));
    CHECK(capture.candidate == shifts);
    capture = {};
    CHECK(!capture.observe({65}));
    CHECK(!capture.observe({66}));
    CHECK(capture.observe({}));
    CHECK(capture.candidate == PttKeyChord{66}); // последовательные A/B не превращаются в A+B
    PttHotkeyState ptt;
    CHECK(!ptt.observe(true, 0, 400));
    CHECK(ptt.pressed);
    CHECK(!ptt.observe(true, 20, 400)); // повтор/автоповтор не считается вторым нажатием
    CHECK(!ptt.observe(false, 50, 400));
    CHECK(!ptt.pressed);
    CHECK(ptt.observe(true, 200, 400));
    ptt.reset(); // Общий замок изменился — история пары сброшена, физическое состояние осталось.
    CHECK(ptt.pressed && ptt.lastPress == -1);
    CHECK(!ptt.observe(false, 250, 400));
    CHECK(!ptt.observe(true, 300, 400));
    ptt.reset(); // Снятие общего замка также не начинает следующую пару.
    CHECK(!ptt.observe(true, 320, 400));
    CHECK(!ptt.observe(false, 350, 400));
    CHECK(!ptt.observe(true, 400, 400));
    CHECK(!ptt.observe(false, 900, 400));
    CHECK(!ptt.observe(true, 950, 400)); // Долгое удержание не становится первым кликом пары.
    ptt.reset();
    CHECK(!ptt.observe(true, 960, 400)); // Остановка/потеря микрофона требуют нового нажатия.
    CHECK(!ptt.observe(false, 1000, 400));
    CHECK(!ptt.observe(true, 1100, 400));
    CHECK(!ptt.observe(false, 1150, 400));
    ptt.reset();
    CHECK(!ptt.observe(true, 1200, 400)); // Reset между кликами также отменяет двойное нажатие.
    CHECK(!ptt.observe(false, 1250, 400));
    CHECK(!ptt.observe(true, 1600, 400)); // Ровно interval — два одиночных нажатия.
    ptt = {};
    CHECK(!ptt.observe(true, 0, 0));
    CHECK(!ptt.observe(false, 50, 0));
    CHECK(!ptt.observe(true, 100, 0)); // Отключённая галочка: двойное нажатие не распознаётся.
    CHECK(!ptt.observe(false, 150, 0));
    std::printf("PTT key chords and double press: %s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
