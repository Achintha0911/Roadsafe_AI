import 'package:http/http.dart' as http;
import 'dart:convert';
import 'dart:typed_data';

class ESP32Service {
  static String? _esp32IP;
  static bool _isConnected = false;

  static String get esp32IP => _esp32IP ?? '';
  static bool get isConnected => _isConnected;

  // Connect to ESP32-CAM
  static Future<bool> connectToESP32(String ip) async {
    try {
      _esp32IP = ip.startsWith('http') ? ip : 'http://$ip';

      final response = await http
          .get(
            Uri.parse('$_esp32IP/status'),
            headers: {'Content-Type': 'application/json'},
          )
          .timeout(Duration(seconds: 5));

      if (response.statusCode == 200) {
        _isConnected = true;
        print('Connected to ESP32-CAM: $_esp32IP');
        return true;
      }
    } catch (e) {
      print('Failed to connect to ESP32: $e');
    }

    _isConnected = false;
    return false;
  }

  // Send alarm command to ESP32
  static Future<bool> sendAlarmCommand(String command, String status) async {
    if (!_isConnected || _esp32IP == null) return false;

    try {
      final response = await http
          .post(
            Uri.parse('$_esp32IP/alarm'),
            headers: {'Content-Type': 'application/json'},
            body: jsonEncode({
              'command': command,
              'status': status,
              'confidence': command == 'ALARM_ON' ? 0.95 : 0.0,
            }),
          )
          .timeout(Duration(seconds: 3));

      return response.statusCode == 200;
    } catch (e) {
      print('Failed to send alarm command: $e');
      return false;
    }
  }

  // Get ESP32 status
  static Future<Map<String, dynamic>?> getStatus() async {
    if (!_isConnected || _esp32IP == null) return null;

    try {
      final response = await http
          .get(Uri.parse('$_esp32IP/status'))
          .timeout(Duration(seconds: 3));

      if (response.statusCode == 200) {
        return jsonDecode(response.body);
      }
    } catch (e) {
      print('Failed to get ESP32 status: $e');
    }

    return null;
  }

  // Get single frame from ESP32 camera
  static Future<Uint8List?> captureFrame() async {
    if (!_isConnected || _esp32IP == null) return null;

    try {
      final response = await http
          .get(Uri.parse('$_esp32IP/capture'))
          .timeout(Duration(seconds: 5));

      if (response.statusCode == 200) {
        return response.bodyBytes;
      }
    } catch (e) {
      print('Failed to capture frame: $e');
    }

    return null;
  }
}
