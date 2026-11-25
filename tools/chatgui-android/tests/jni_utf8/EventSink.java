/** Настоящий Java callback для проверки регистрации, замены и исключений доставки JNI. */
public final class EventSink {
    public String getString(String key) {
        if (key.equals("throw")) throw new IllegalStateException("injected config failure");
        return key + " Имя 😀";
    }
    public int calls;
    public int type;
    public int bytes;
    public boolean fail;
    public void onEvent(int eventType, byte[] data) {
        calls++;
        type = eventType;
        bytes = data == null ? 0 : data.length;
        if (fail) throw new IllegalStateException("injected event callback failure");
    }
}
