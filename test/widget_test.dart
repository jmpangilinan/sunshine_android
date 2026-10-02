import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:sunshine_android/main.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('com.nightmare.sunshine');
  const codec = StandardMethodCodec();
  late Completer<Object?> start;

  Future<void> emit(String method, Map<String, Object?> state) async {
    await TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .handlePlatformMessage(channel.name, codec.encodeMethodCall(MethodCall(method, state)), (_) {});
  }

  setUp(() {
    start = Completer<Object?>();
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (call) async {
      switch (call.method) {
        case 'getHostState':
          return {
            'state': 'IDLE',
            'reason': '',
            'input': {'mode': 2, 'ready': true, 'uid': 2000, 'maxControllers': 0},
          };
        case 'address':
          return '192.168.1.10';
        case 'start':
          final response = await start.future;
          if (response is PlatformException) throw response;
          return response;
        case 'stop':
          return null;
        default:
          throw PlatformException(code: 'UNEXPECTED_METHOD', message: call.method);
      }
    });
  });

  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, null);
  });

  testWidgets('Start waits for running and Stop follows actual lifecycle', (tester) async {
    await tester.pumpWidget(const MyApp());
    await tester.pumpAndSettle();
    await tester.tap(find.text('Start Server'));
    await tester.pump();
    expect(find.text('Host: IDLE'), findsOneWidget);
    expect(find.text('Starting…'), findsOneWidget);
    final startingButton = tester.widget<ElevatedButton>(find.widgetWithText(ElevatedButton, 'Starting…'));
    expect(startingButton.onPressed, isNull);

    await emit('hostState', {'state': 'STARTING', 'reason': ''});
    await tester.pump();
    expect(find.text('Host: STARTING'), findsOneWidget);
    await emit('hostState', {'state': 'RUNNING', 'reason': ''});
    await tester.runAsync(() async {
      start.complete({'state': 'RUNNING'});
      await Future<void>.delayed(Duration.zero);
    });
    await tester.pumpAndSettle();
    expect(find.text('Host: RUNNING'), findsOneWidget);
    expect(tester.widget<OutlinedButton>(find.widgetWithText(OutlinedButton, 'Stop Server')).onPressed, isNotNull);

    await tester.tap(find.text('Stop Server'));
    await tester.pump();
    // A successful stop request alone is not proof the service stopped.
    expect(find.text('Host: RUNNING'), findsOneWidget);
    await emit('hostState', {'state': 'STOPPING', 'reason': ''});
    await tester.pump();
    expect(tester.widget<OutlinedButton>(find.widgetWithText(OutlinedButton, 'Stop Server')).onPressed, isNull);
    await emit('hostState', {'state': 'IDLE', 'reason': ''});
    await tester.pumpAndSettle();
    expect(find.text('Host: IDLE'), findsOneWidget);
    expect(tester.widget<ElevatedButton>(find.widgetWithText(ElevatedButton, 'Start Server')).onPressed, isNotNull);
  });

  testWidgets('Declined consent never presents a running host and permits retry', (tester) async {
    await tester.pumpWidget(const MyApp());
    await tester.pumpAndSettle();
    await tester.tap(find.text('Start Server'));
    await tester.pump();
    await emit('hostState', {'state': 'FAILED', 'reason': 'Screen capture consent declined'});
    await tester.runAsync(() async {
      start.complete(PlatformException(code: 'CONSENT_DENIED', message: 'Screen capture consent declined'));
      await Future<void>.delayed(Duration.zero);
    });
    await tester.pumpAndSettle();
    expect(find.text('Host: FAILED'), findsOneWidget);
    expect(find.text('Host: RUNNING'), findsNothing);
    expect(find.text('Screen capture consent declined'), findsWidgets);
    expect(tester.widget<ElevatedButton>(find.widgetWithText(ElevatedButton, 'Start Server')).onPressed, isNotNull);
    expect(tester.widget<OutlinedButton>(find.widgetWithText(OutlinedButton, 'Stop Server')).onPressed, isNull);
  });

  testWidgets('Failed host cannot restart until owned teardown completes', (tester) async {
    await tester.pumpWidget(const MyApp());
    await tester.pumpAndSettle();
    await emit('hostState', {'state': 'FAILED', 'reason': 'Encoder failed', 'busy': true});
    await tester.pumpAndSettle();
    expect(find.text('Host: FAILED'), findsOneWidget);
    expect(tester.widget<ElevatedButton>(find.widgetWithText(ElevatedButton, 'Start Server')).onPressed, isNull);
    await emit('hostState', {'state': 'FAILED', 'reason': 'Encoder failed', 'busy': false});
    await tester.pumpAndSettle();
    expect(tester.widget<ElevatedButton>(find.widgetWithText(ElevatedButton, 'Start Server')).onPressed, isNotNull);
  });

  testWidgets('Backend loss makes Start unavailable and displays its reason', (tester) async {
    await tester.pumpWidget(const MyApp());
    await tester.pumpAndSettle();
    await emit('inputState', {
      'mode': 2,
      'ready': false,
      'uid': 2000,
      'maxControllers': 0,
      'failureReason': 'Shizuku service disconnected',
    });
    await tester.pumpAndSettle();
    expect(find.text('Controls not ready'), findsOneWidget);
    expect(find.text('Shizuku service disconnected'), findsOneWidget);
    expect(tester.widget<ElevatedButton>(find.widgetWithText(ElevatedButton, 'Start Server')).onPressed, isNull);
  });
}
