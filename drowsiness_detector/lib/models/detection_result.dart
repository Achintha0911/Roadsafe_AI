class DetectionResult {
  final bool isDrowsy;
  final double confidence;
  final DateTime timestamp;
  final String status;

  DetectionResult({
    required this.isDrowsy,
    required this.confidence,
    required this.timestamp,
    required this.status,
  });

  factory DetectionResult.fromJson(Map<String, dynamic> json) {
    return DetectionResult(
      isDrowsy: json['isDrowsy'] ?? false,
      confidence: (json['confidence'] ?? 0.0).toDouble(),
      timestamp: DateTime.now(),
      status: json['status'] ?? 'Normal',
    );
  }

  Map<String, dynamic> toJson() {
    return {
      'isDrowsy': isDrowsy,
      'confidence': confidence,
      'timestamp': timestamp.toIso8601String(),
      'status': status,
    };
  }
}
