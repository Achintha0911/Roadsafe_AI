import 'package:flutter_local_notifications/flutter_local_notifications.dart';

class NotificationService {
  static final FlutterLocalNotificationsPlugin _notifications =
      FlutterLocalNotificationsPlugin();

  static Future<void> initialize() async {
    const AndroidInitializationSettings initializationSettingsAndroid =
        AndroidInitializationSettings('@mipmap/ic_launcher');

    const DarwinInitializationSettings initializationSettingsIOS =
        DarwinInitializationSettings();

    const InitializationSettings initializationSettings =
        InitializationSettings(
          android: initializationSettingsAndroid,
          iOS: initializationSettingsIOS,
        );

    await _notifications.initialize(initializationSettings);
  }

  static Future<void> showDrowsinessAlert() async {
    const AndroidNotificationDetails androidPlatformChannelSpecifics =
        AndroidNotificationDetails(
          'drowsiness_alerts',
          'Drowsiness Alerts',
          channelDescription: 'Alerts for drowsiness detection',
          importance: Importance.max,
          priority: Priority.high,
          ticker: 'DROWSINESS ALERT!',
        );

    const NotificationDetails platformChannelSpecifics = NotificationDetails(
      android: androidPlatformChannelSpecifics,
    );

    await _notifications.show(
      0,
      '⚠️ DROWSINESS ALERT!',
      'Driver drowsiness detected! Please pull over safely.',
      platformChannelSpecifics,
    );
  }
}
