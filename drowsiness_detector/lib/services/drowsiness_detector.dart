import 'dart:typed_data';
import 'dart:io';
import 'package:tflite_flutter/tflite_flutter.dart';
import 'package:image/image.dart' as img;
import '../models/detection_result.dart';

class DrowsinessDetector {
  static Interpreter? _interpreter;
  static bool _isInitialized = false;

  // Initialize the TensorFlow Lite model
  static Future<bool> initialize() async {
    try {
      // Load the model from assets
      _interpreter = await Interpreter.fromAsset(
        'assets/model/drowsiness_model.tflite',
      );
      _isInitialized = true;
      print('Drowsiness detection model loaded successfully');
      return true;
    } catch (e) {
      print('Failed to load model: $e');
      return false;
    }
  }

  // Process image and detect drowsiness
  static Future<DetectionResult?> detectDrowsiness(Uint8List imageBytes) async {
    if (!_isInitialized || _interpreter == null) {
      await initialize();
      if (!_isInitialized) return null;
    }

    try {
      // Decode image
      img.Image? image = img.decodeImage(imageBytes);
      if (image == null) return null;

      // Resize image to match model input (adjust size based on your model)
      image = img.copyResize(image, width: 224, height: 224);

      // Convert to Float32List for model input
      var input = _imageToByteListFloat32(image, 224, 224);

      // Prepare output tensor
      var output = List.filled(1 * 2, 0.0).reshape([1, 2]); // [Normal, Drowsy]

      // Run inference
      _interpreter!.run(input, output);

      // Process results
      double normalConfidence = output[0][0];
      double drowsyConfidence = output[0][1];

      bool isDrowsy = drowsyConfidence > normalConfidence;
      double confidence = isDrowsy ? drowsyConfidence : normalConfidence;

      return DetectionResult(
        isDrowsy: isDrowsy,
        confidence: confidence,
        timestamp: DateTime.now(),
        status: isDrowsy ? 'Drowsy' : 'Normal',
      );
    } catch (e) {
      print('Error during inference: $e');
      return null;
    }
  }

  // Convert image to Float32List for model input
  static Float32List _imageToByteListFloat32(
    img.Image image,
    int width,
    int height,
  ) {
    var convertedBytes = Float32List(1 * width * height * 3);
    var buffer = Float32List.view(convertedBytes.buffer);
    int pixelIndex = 0;

    for (int i = 0; i < height; i++) {
      for (int j = 0; j < width; j++) {
        var pixel = image.getPixel(j, i);
        buffer[pixelIndex++] = img.getRed(pixel) / 255.0;
        buffer[pixelIndex++] = img.getGreen(pixel) / 255.0;
        buffer[pixelIndex++] = img.getBlue(pixel) / 255.0;
      }
    }
    return convertedBytes.reshape([1, width, height, 3]);
  }

  // Dispose resources
  static void dispose() {
    _interpreter?.close();
    _interpreter = null;
    _isInitialized = false;
  }
}
