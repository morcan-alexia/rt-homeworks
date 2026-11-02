package ssatr.service;

import java.util.Map;
import java.util.TreeMap;

/**
 * Ultima stare cunoscută a fiecărui device: ultima temperatură, momentul
 * ultimului mesaj, numărul de alarme etc.
 *
 * Este actualizată de thread-urile MessageProcessor și citită de CommandConsole,
 * deci accesul trebuie sincronizat.
 */
public class Dashboard {

    private static class DeviceState {
        double lastTemperature;
        long lastMessageMillis;
        int alarmCount;
    }

    private final Map<String, DeviceState> devices = new TreeMap<>();

    private DeviceState stateOf(String deviceId) {
        DeviceState state = devices.computeIfAbsent(deviceId, id -> new DeviceState());
        state.lastMessageMillis = System.currentTimeMillis();
        return state;
    }

    public synchronized void updateTelemetry(String deviceId, double temperature) {
        stateOf(deviceId).lastTemperature = temperature;
    }

    public synchronized void recordAlarm(String deviceId) {
        stateOf(deviceId).alarmCount++;
    }

    public synchronized void updateStatus(String deviceId) {
        stateOf(deviceId);
    }


    public synchronized void print() {

        long now = System.currentTimeMillis();
        System.out.printf("%-12s %8s  %-15s %6s%n", "DEVICE", "TEMP", "ULTIMUL MESAJ", "ALARME");
        for (Map.Entry<String, DeviceState> entry : devices.entrySet()) {
            DeviceState s = entry.getValue();
            long ageSeconds = (now - s.lastMessageMillis) / 1000;
            System.out.printf("%-12s %8.2f  %-15s %6d%n",
                    entry.getKey(), s.lastTemperature, "acum " + ageSeconds + " s", s.alarmCount);
        }
    }
}
