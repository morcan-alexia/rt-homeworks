package ssatr.service;

import java.util.concurrent.BlockingQueue;

/**
 * Preia mesajele de un anumit tip ("telemetry", "alarm" sau "status") din coada
 * inbox, le salvează și actualizează dashboard-ul.
 *
 * Formatul mesajelor (câmpuri separate prin ";"):
 *   telemetry:  deviceId;timestamp;temperatura
 *   alarm:      deviceId;timestamp;temperatura;prag
 *   status:     deviceId;timestamp;text
 */
public class MessageProcessor implements Runnable {

    private final String kind;
    private final BlockingQueue<String> inbox;
    private final CsvStorage storage;
    private final Dashboard dashboard;

    public MessageProcessor(String kind, BlockingQueue<String> inbox, CsvStorage storage, Dashboard dashboard) {
        this.kind = kind;
        this.inbox = inbox;
        this.storage = storage;
        this.dashboard = dashboard;
    }

    @Override
    public void run() {

        try {
            while (true) {
                String message = inbox.take();
                try {
                    String[] f = message.split(";");
                    String deviceId = f[0];

                    switch (kind) {
                        case "telemetry":
                            double temperature = Double.parseDouble(f[2]);
                            storage.append(kind, message);
                            dashboard.updateTelemetry(deviceId, temperature);
                            break;

                        case "alarm":
                            String temp = f[2];
                            String threshold = f[3];
                            storage.append(kind, message);
                            dashboard.recordAlarm(deviceId);
                            System.out.println("!!! ALARMĂ " + deviceId + ": " + temp + " > prag " + threshold);
                            break;

                        case "status":
                            String text = f[2];
                            storage.append(kind, message);
                            dashboard.updateStatus(deviceId);
                            System.out.println(">>> " + deviceId + ": " + text);
                            break;
                    }
                } catch (RuntimeException e) {

                    System.out.println("[" + kind + "] mesaj ignorat: " + message);
                }
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }
}
