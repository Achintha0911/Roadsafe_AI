import 'dart:async';
import 'dart:typed_data';
import 'package:flutter/material.dart';
import '../services/esp32_services.dart';
import '../services/drowsiness_detector.dart';
import '../services/notification_service.dart';
import '../models/detection_result.dart';

class CameraScreen extends StatefulWidget {
  @override
  _CameraScreenState createState() => _CameraScreenState();
}

class _CameraScreenState extends State<CameraScreen> {
  Timer? _detectionTimer;
  DetectionResult? _lastResult;
  bool _isProcessing = false;
  int _drowsinessCount = 0;
  int _totalAlerts = 0;
  Uint8List? _currentFrame;

  @override
  void initState() {
    super.initState();
    _startDetectionLoop();
  }

  void _startDetectionLoop() {
    _detectionTimer = Timer.periodic(Duration(seconds: 2), (timer) {
      _processFrame();
    });
  }

  Future<void> _processFrame() async {
    if (_isProcessing) return;

    setState(() {
      _isProcessing = true;
    });

    try {
      // Capture frame from ESP32-CAM
      Uint8List? frameBytes = await ESP32Service.captureFrame();

      if (frameBytes != null) {
        setState(() {
          _currentFrame = frameBytes;
        });

        // Detect drowsiness
        DetectionResult? result = await DrowsinessDetector.detectDrowsiness(
          frameBytes,
        );

        if (result != null) {
          setState(() {
            _lastResult = result;
          });

          // Handle drowsiness detection
          if (result.isDrowsy && result.confidence > 0.7) {
            _drowsinessCount++;

            // Trigger alarm after 2 consecutive drowsy detections
            if (_drowsinessCount >= 2) {
              _triggerDrowsinessAlert();
              _drowsinessCount = 0; // Reset counter
            }
          } else {
            _drowsinessCount = 0; // Reset if not drowsy
            // Turn off alarm if it was on
            await ESP32Service.sendAlarmCommand('ALARM_OFF', 'Normal');
          }
        }
      }
    } catch (e) {
      print('Error processing frame: $e');
    } finally {
      setState(() {
        _isProcessing = false;
      });
    }
  }

  Future<void> _triggerDrowsinessAlert() async {
    _totalAlerts++;

    // Send alarm command to ESP32
    bool success = await ESP32Service.sendAlarmCommand('ALARM_ON', 'Drowsy');

    if (success) {
      // Show local notification
      await NotificationService.showDrowsinessAlert();

      // Auto-stop alarm after 10 seconds
      Timer(Duration(seconds: 10), () async {
        await ESP32Service.sendAlarmCommand('ALARM_OFF', 'Normal');
      });
    }

    // Show dialog
    if (mounted) {
      showDialog(
        context: context,
        barrierDismissible: false,
        builder: (BuildContext context) {
          return AlertDialog(
            title: Text('⚠️ DROWSINESS ALERT!'),
            content: Text(
              'Driver drowsiness detected!\nPlease pull over safely.',
            ),
            backgroundColor: Colors.red[50],
            actions: [
              TextButton(
                child: Text('I\'m Awake', style: TextStyle(fontSize: 16)),
                onPressed: () {
                  Navigator.of(context).pop();
                  ESP32Service.sendAlarmCommand('ALARM_OFF', 'Normal');
                },
              ),
            ],
          );
        },
      );
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text('Live Detection'),
        backgroundColor: Colors.blue[800],
        foregroundColor: Colors.white,
        actions: [
          IconButton(
            icon: Icon(Icons.refresh),
            onPressed: () => _processFrame(),
          ),
        ],
      ),
      body: Column(
        children: [
          // Status Bar
          Container(
            width: double.infinity,
            padding: EdgeInsets.all(16),
            color: _lastResult?.isDrowsy == true
                ? Colors.red[100]
                : Colors.green[100],
            child: Row(
              mainAxisAlignment: MainAxisAlignment.spaceBetween,
              children: [
                Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      'Status: ${_lastResult?.status ?? 'Detecting...'}',
                      style: TextStyle(
                        fontSize: 18,
                        fontWeight: FontWeight.bold,
                        color: _lastResult?.isDrowsy == true
                            ? Colors.red[800]
                            : Colors.green[800],
                      ),
                    ),
                    if (_lastResult != null)
                      Text(
                        'Confidence: ${(_lastResult!.confidence * 100).toStringAsFixed(1)}%',
                        style: TextStyle(fontSize: 14),
                      ),
                  ],
                ),
                Column(
                  crossAxisAlignment: CrossAxisAlignment.end,
                  children: [
                    Text('Total Alerts: $_totalAlerts'),
                    Text('Processing: ${_isProcessing ? 'Yes' : 'No'}'),
                  ],
                ),
              ],
            ),
          ),

          // Camera Feed
          Expanded(
            child: Container(
              width: double.infinity,
              child: _currentFrame != null
                  ? Image.memory(_currentFrame!, fit: BoxFit.contain)
                  : Center(
                      child: Column(
                        mainAxisAlignment: MainAxisAlignment.center,
                        children: [
                          CircularProgressIndicator(),
                          SizedBox(height: 20),
                          Text('Loading camera feed...'),
                        ],
                      ),
                    ),
            ),
          ),

          // Control Buttons
          Container(
            padding: EdgeInsets.all(16),
            child: Row(
              mainAxisAlignment: MainAxisAlignment.spaceEvenly,
              children: [
                ElevatedButton.icon(
                  onPressed: () async {
                    await ESP32Service.sendAlarmCommand(
                      'ALARM_OFF',
                      'Manual Stop',
                    );
                  },
                  icon: Icon(Icons.stop),
                  label: Text('Stop Alarm'),
                  style: ElevatedButton.styleFrom(
                    backgroundColor: Colors.red[600],
                    foregroundColor: Colors.white,
                  ),
                ),
                ElevatedButton.icon(
                  onPressed: _processFrame,
                  icon: Icon(Icons.refresh),
                  label: Text('Refresh'),
                  style: ElevatedButton.styleFrom(
                    backgroundColor: Colors.blue[600],
                    foregroundColor: Colors.white,
                  ),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  @override
  void dispose() {
    _detectionTimer?.cancel();
    super.dispose();
  }
}
