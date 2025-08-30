import 'dart:typed_data';
import 'dart:convert';
import 'package:http/http.dart' as http;
import '../models/detection_result.dart';

class DrowsinessDetector {
  // YOUR ACTUAL ROBOFLOW API DETAILS
  static const String API_KEY = "kU0QoAFfW5QbD4uwb3p1";
  static const String API_URL = "https://serverless.roboflow.com";
  static const String MODEL_ID = "drowsiness-driver/1";

  static bool _isInitialized = true; // API doesn't need model loading

  // Initialize (just for compatibility)
  static Future<bool> initialize() async {
    print('Using Roboflow Hosted API for drowsiness detection');
    print('API URL: $API_URL');
    print('Model: $MODEL_ID');
    return true;
  }

  // Process image using YOUR Roboflow API
  static Future<DetectionResult?> detectDrowsiness(Uint8List imageBytes) async {
    try {
      // Convert image to base64
      String base64Image = base64Encode(imageBytes);

      // Prepare API request using Roboflow format
      var request = http.MultipartRequest(
        'POST',
        Uri.parse('$API_URL/$MODEL_ID?api_key=$API_KEY'),
      );

      // Add the image as base64
      request.fields['image'] = base64Image;
      request.fields['confidence'] = '0.4'; // 40% confidence threshold
      request.fields['overlap'] = '30'; // Overlap threshold

      print('Sending image to Roboflow API...');

      // Make API call
      var streamedResponse = await request.send().timeout(
        Duration(seconds: 15),
      );
      var response = await http.Response.fromStream(streamedResponse);

      if (response.statusCode == 200) {
        Map<String, dynamic> result = jsonDecode(response.body);

        print('Roboflow response: ${response.body}');

        // Parse Roboflow response
        List<dynamic> predictions = result['predictions'] ?? [];

        // Analyze predictions for drowsiness
        bool isDrowsy = false;
        double maxConfidence = 0.0;
        String detectedClass = 'Normal';
        int drowsyCount = 0;
        int alertCount = 0;

        print('Found ${predictions.length} predictions');

        for (var prediction in predictions) {
          String className = prediction['class'].toString().toLowerCase();
          double confidence = prediction['confidence'].toDouble();

          print(
            'Detection: $className (${(confidence * 100).toStringAsFixed(1)}%)',
          );

          // Check for drowsiness indicators based on your model classes
          if (className.contains('drowsy') ||
              className.contains('sleepy') ||
              className.contains('tired') ||
              className.contains('closed') ||
              className.contains('yawn') ||
              className.contains('fatigue')) {
            isDrowsy = true;
            drowsyCount++;

            if (confidence > maxConfidence) {
              maxConfidence = confidence;
              detectedClass = prediction['class'];
            }
          } else if (className.contains('alert') ||
              className.contains('awake') ||
              className.contains('normal')) {
            alertCount++;

            if (confidence > maxConfidence && !isDrowsy) {
              maxConfidence = confidence;
              detectedClass = prediction['class'];
            }
          }
        }

        // Decision logic
        if (predictions.isEmpty) {
          // No face detected
          maxConfidence = 0.3;
          detectedClass = 'No Face Detected';
          isDrowsy = false;
        } else if (drowsyCount > alertCount) {
          // More drowsy detections than alert
          isDrowsy = true;
        } else if (alertCount > 0) {
          // Alert state detected
          isDrowsy = false;
        }

        print(
          'Final Decision: ${isDrowsy ? "DROWSY" : "ALERT"} - $detectedClass (${(maxConfidence * 100).toStringAsFixed(1)}%)',
        );

        return DetectionResult(
          isDrowsy: isDrowsy,
          confidence: maxConfidence,
          timestamp: DateTime.now(),
          status: isDrowsy
              ? 'Drowsy - $detectedClass'
              : 'Alert - $detectedClass',
        );
      } else {
        print('Roboflow API Error: ${response.statusCode}');
        print('Response body: ${response.body}');
        return null;
      }
    } catch (e) {
      print('Error calling Roboflow API: $e');
      return null;
    }
  }

  // Test API connection with a small image
  static Future<bool> testConnection() async {
    try {
      print('Testing Roboflow API connection...');

      var request = http.MultipartRequest(
        'POST',
        Uri.parse('$API_URL/$MODEL_ID?api_key=$API_KEY'),
      );

      // Create a minimal test image (black square)
      String testBase64 =
          "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==";
      request.fields['image'] = testBase64;

      var streamedResponse = await request.send().timeout(
        Duration(seconds: 10),
      );
      var response = await http.Response.fromStream(streamedResponse);

      if (response.statusCode == 200) {
        print('API Connection Test: SUCCESS');
        print('Response: ${response.body}');
        return true;
      } else {
        print('API Connection Test: FAILED - Status ${response.statusCode}');
        print('Response: ${response.body}');
        return false;
      }
    } catch (e) {
      print('Connection test failed: $e');
      return false;
    }
  }

  // Dispose (nothing to dispose for API)
  static void dispose() {
    print('Roboflow API detector disposed');
  }
}
