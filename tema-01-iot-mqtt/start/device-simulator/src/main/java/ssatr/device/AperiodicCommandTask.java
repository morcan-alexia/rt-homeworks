package ssatr.device;

import java.util.concurrent.BlockingQueue;

/**
 * TASK APERIODIC: execută comenzile primite de la business service.
 *
 * Comenzile sosesc oricând, pe MQTT, fără un interval minim între ele, și sunt
 * puse în coada commands. Fiecare comandă eliberează un job al taskului.
 * Comenzi care trebuie suportate:
 *
 *   SET_INTERVAL <ms>       schimbă perioada T a taskului periodic
 *   SET_THRESHOLD <valoare> schimbă pragul de alarmă
 *   RESET                   readuce senzorul la valoarea inițială
 *   STATUS                  raportează perioada și pragul curente
 *
 * După executarea fiecărei comenzi, device-ul publică pe Config.statusTopic(deviceId)
 * o confirmare: "deviceId;timestamp;text", de exemplu
 * "device-1;1727600000000;OK SET_INTERVAL 1000".
 */
public class AperiodicCommandTask implements Runnable {

    private final String deviceId;
    private final TemperatureSensor sensor;
    private final MqttConnection mqtt;
    private final BlockingQueue<String> commands;
    private final PeriodicTelemetryTask periodic;

    public AperiodicCommandTask(String deviceId, TemperatureSensor sensor, MqttConnection mqtt,
                                BlockingQueue<String> commands, PeriodicTelemetryTask periodic) {
        this.deviceId = deviceId;
        this.sensor = sensor;
        this.mqtt = mqtt;
        this.commands = commands;
        this.periodic = periodic;
    }

    @Override
    public void run() {
        try {
            while (true) {
                String command = commands.take();
                String result = execute(command.trim());

                String message = deviceId + ";" + System.currentTimeMillis() + ";" + result;
                mqtt.publish(Config.statusTopic(deviceId), message);
                System.out.println("      [aperiodic] " + command + " -> " + result);
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }


    private String execute(String command) {
        String[] parts = command.split("\\s+");
        String name = parts[0];
        try {
            switch (name) {
                case "SET_INTERVAL":
                    long ms = Long.parseLong(parts[1]);
                    if (ms < 100) {
                        return "ERROR SET_INTERVAL: minim 100 ms";
                    }
                    periodic.setPeriodMs(ms);
                    return "OK SET_INTERVAL " + ms;

                case "SET_THRESHOLD":
                    double value = Double.parseDouble(parts[1]);
                    periodic.setThreshold(value);
                    return "OK SET_THRESHOLD " + value;

                case "RESET":
                    sensor.reset();
                    return "OK RESET";

                case "STATUS":
                    return "OK STATUS interval=" + periodic.getPeriodMs()
                            + " threshold=" + periodic.getThreshold();

                default:
                    return "ERROR comanda necunoscuta: " + name;
            }
        } catch (ArrayIndexOutOfBoundsException e) {
            return "ERROR " + name + ": lipseste parametrul";      // ex. "SET_INTERVAL" fără număr
        } catch (NumberFormatException e) {
            return "ERROR " + name + ": parametru invalid";        // ex. "SET_INTERVAL abc"
        }
    }
}
