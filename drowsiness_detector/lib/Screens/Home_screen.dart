import 'package:flutter/material.dart';
import 'camera_screens.dart';
import '../services/esp32_services.dart';
import '../services/drowsiness_detector.dart';

class HomeScreen extends StatefulWidget {
  @override
  _HomeScreenState createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  final TextEditingController _ipController = TextEditingController();
  bool _isConnecting = false;
  bool _isConnected = false;
  String _statusMessage = 'Enter ESP32-CAM IP address';

  @override
  void initState() {
    super.initState();
    _initializeServices();
  }

  Future<void> _initializeServices() async {
    // Initialize drowsiness detection model
    bool modelLoaded = await DrowsinessDetector.initialize();
    if (!modelLoaded) {
      setState(() {
        _statusMessage = 'Failed to load drowsiness detection model';
      });
    }
  }

  Future<void> _connectToESP32() async {
    if (_ipController.text.isEmpty) {
      _showSnackBar('Please enter ESP32-CAM IP address');
      return;
    }

    setState(() {
      _isConnecting = true;
      _statusMessage = 'Connecting to ESP32-CAM...';
    });

    bool connected = await ESP32Service.connectToESP32(_ipController.text);

    setState(() {
      _isConnecting = false;
      _isConnected = connected;
      _statusMessage = connected
          ? 'Connected to ESP32-CAM successfully!'
          : 'Failed to connect to ESP32-CAM';
    });

    if (connected) {
      _showSnackBar('Connected successfully!');
    } else {
      _showSnackBar('Connection failed. Check IP address and network.');
    }
  }

  void _showSnackBar(String message) {
    ScaffoldMessenger.of(
      context,
    ).showSnackBar(SnackBar(content: Text(message)));
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text('Drowsiness Detector'),
        backgroundColor: Colors.blue[800],
        foregroundColor: Colors.white,
      ),
      body: Padding(
        padding: EdgeInsets.all(20.0),
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            Icon(Icons.camera_alt, size: 100, color: Colors.blue[800]),
            SizedBox(height: 30),

            Text(
              'ESP32-CAM Drowsiness Detection',
              style: TextStyle(
                fontSize: 24,
                fontWeight: FontWeight.bold,
                color: Colors.blue[800],
              ),
              textAlign: TextAlign.center,
            ),

            SizedBox(height: 40),

            TextField(
              controller: _ipController,
              decoration: InputDecoration(
                labelText: 'ESP32-CAM IP Address',
                hintText: '192.168.1.100',
                prefixIcon: Icon(Icons.wifi),
                border: OutlineInputBorder(
                  borderRadius: BorderRadius.circular(10),
                ),
              ),
              keyboardType: TextInputType.url,
            ),

            SizedBox(height: 20),

            Container(
              padding: EdgeInsets.all(15),
              decoration: BoxDecoration(
                color: _isConnected ? Colors.green[50] : Colors.orange[50],
                border: Border.all(
                  color: _isConnected ? Colors.green : Colors.orange,
                ),
                borderRadius: BorderRadius.circular(10),
              ),
              child: Row(
                children: [
                  Icon(
                    _isConnected ? Icons.check_circle : Icons.info,
                    color: _isConnected ? Colors.green : Colors.orange,
                  ),
                  SizedBox(width: 10),
                  Expanded(
                    child: Text(
                      _statusMessage,
                      style: TextStyle(
                        color: _isConnected
                            ? Colors.green[800]
                            : Colors.orange[800],
                      ),
                    ),
                  ),
                ],
              ),
            ),

            SizedBox(height: 30),

            ElevatedButton(
              onPressed: _isConnecting ? null : _connectToESP32,
              child: _isConnecting
                  ? Row(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        SizedBox(
                          width: 20,
                          height: 20,
                          child: CircularProgressIndicator(strokeWidth: 2),
                        ),
                        SizedBox(width: 10),
                        Text('Connecting...'),
                      ],
                    )
                  : Text('Connect to ESP32-CAM'),
              style: ElevatedButton.styleFrom(
                backgroundColor: Colors.blue[800],
                foregroundColor: Colors.white,
                padding: EdgeInsets.symmetric(horizontal: 30, vertical: 15),
                textStyle: TextStyle(fontSize: 16),
              ),
            ),

            SizedBox(height: 20),

            ElevatedButton(
              onPressed: _isConnected
                  ? () async {
                      // Test API first
                      _showSnackBar('Testing Roboflow API...');
                      bool apiWorks = await DrowsinessDetector.testConnection();

                      if (apiWorks) {
                        _showSnackBar(
                          'API test successful! Starting detection...',
                        );
                        Navigator.push(
                          context,
                          MaterialPageRoute(
                            builder: (context) => CameraScreen(),
                          ),
                        );
                      } else {
                        _showSnackBar(
                          'API test failed. Check your internet connection.',
                        );
                      }
                    }
                  : null,
              child: Text('Start Detection'),
              style: ElevatedButton.styleFrom(
                backgroundColor: Colors.green[600],
                foregroundColor: Colors.white,
                padding: EdgeInsets.symmetric(horizontal: 30, vertical: 15),
                textStyle: TextStyle(fontSize: 16),
              ),
            ),
          ],
        ),
      ),
    );
  }

  @override
  void dispose() {
    _ipController.dispose();
    super.dispose();
  }
}
