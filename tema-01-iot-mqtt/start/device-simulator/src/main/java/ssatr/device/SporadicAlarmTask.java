package ssatr.device;

import java.util.Locale;
import java.util.concurrent.BlockingQueue;

/**
 * TASK SPORADIC: tratează alarmele de temperatură.
 *
 * Evenimentele sosesc în coada alarmEvents (puse de taskul periodic).
 * Intervalul minim între sosiri este MIN_INTERARRIVAL_MS: între două eliberări
 * ale taskului (două alarme publicate) trebuie să treacă cel puțin atât.
 * Intervalul este impus de cod: evenimentele care sosesc mai des sunt
 * respinse, ca să nu fie inundat brokerul.
 */
public class SporadicAlarmTask implements Runnable {

    static final long MIN_INTERARRIVAL_MS = 5000;

    private final String deviceId;
    private final MqttConnection mqtt;
    private final BlockingQueue<Double> alarmEvents;

    private final PeriodicTelemetryTask periodic;

    public SporadicAlarmTask(String deviceId, MqttConnection mqtt, BlockingQueue<Double> alarmEvents,
                             PeriodicTelemetryTask periodic) {
        this.deviceId = deviceId;
        this.mqtt = mqtt;
        this.alarmEvents = alarmEvents;
        this.periodic = periodic;
    }

    /*
     * Pragul publicat în alarma e citit din PeriodicTelemetryTask (getThreshold())
     * in momentul publicarii, nu e transmis prin coada.
     * Avantaj: cod simplu, coada transporta doar temperatura.
     * Dezavantaj: dacă intre detectare si publicare sosește SET_THRESHOLD, alarma
     * raporteaza pragul nou. Fereastra e de cațiva ms, deci riscul e mic.
     */

    @Override
    public void run() {
        long lastRelease = 0;
        try {
            while (true) {
                double temperature = alarmEvents.take();
                long now = System.currentTimeMillis();

                long sinceLast = now - lastRelease;
                if (sinceLast < MIN_INTERARRIVAL_MS) {
                    System.out.println("   [sporadic] RESPINS: au trecut doar " + sinceLast + " ms");
                    continue;
                }
                lastRelease = now;

                String message = deviceId + ";" + now + ";"
                        + String.format(Locale.US, "%.2f;%.2f", temperature, periodic.getThreshold());
                mqtt.publish(Config.alarmTopic(deviceId), message);
                System.out.println("   [sporadic] ALARMĂ publicată: " + message);

            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }
}
