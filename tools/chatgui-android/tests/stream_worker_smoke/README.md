# Проверка независимости аудио и видео на устройстве

Из корня репозитория после сборки `assembleDebug`:

```bash
python3 tools/chatgui-android/tests/stream_worker_smoke/run.py SERIAL > /tmp/stream-worker-smoke.log 2>&1
```

Нужны подключённое ADB-устройство, JDK, FFmpeg с libx264, Android SDK API 36 и build-tools.
Путь SDK берётся из `--sdk`, `ANDROID_HOME`, `ANDROID_SDK_ROOT` или `local.properties` проекта.
Другой собранный APK можно передать через `--apk`.

Тест запускает классы собранного APK в отдельном `app_process`, без переустановки приложения.
Он использует настоящий MediaCodec с ImageReader Surface, native Opus/SoundTouch, StreamMixedPlayback
и общий AudioTrack. Аудиоматериал — цифровая тишина.

Проверки:

- Callback показа блокирует video worker на 500 мс. За это время аппаратные аудиочасы должны
  пройти минимум 20 000 кадров, а `underruns` не должен увеличиться.
- Все 120 видеокадров декодируются до EOF; поздние после остановки кадры разрешено отбросить.
- Тот же video worker после reset декодирует короткую трансляцию из трёх кадров и снова завершает EOF.
- После каждого сценария дожидаемся физического завершения аудиохвоста и освобождаем input.

Успех отмечается строками `PASS`; нарушение инвариантов завершает процесс с ненулевым кодом.
Подробные сообщения production workers доступны в `adb logcat -s utun-gui:D`.
Тестовые APK, библиотеки и fixture остаются в `/data/local/tmp/utun-stream-worker-smoke` для диагностики.
Сетевой путь и UI этот сценарий не проверяет.
