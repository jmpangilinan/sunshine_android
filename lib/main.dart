import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

void main() => runApp(const MyApp());

class MyApp extends StatelessWidget {
  const MyApp({super.key});

  @override
  Widget build(BuildContext context) => MaterialApp(
        title: 'Sunshine',
        theme: ThemeData(
          colorScheme: ColorScheme.fromSeed(seedColor: Colors.blue),
          useMaterial3: true,
        ),
        home: const HostPage(),
      );
}

class HostPage extends StatefulWidget {
  const HostPage({super.key});

  @override
  State<HostPage> createState() => _HostPageState();
}

class _HostPageState extends State<HostPage> {
  static const _channel = MethodChannel('com.nightmare.sunshine');
  final _pin = TextEditingController();
  String _address = '';
  String _state = 'IDLE';
  String _reason = '';
  bool _audioAvailable = false;
  bool _busy = false;
  String _error = '';
  int _mode = 2;
  Map<Object?, Object?> _input = {};
  bool _startPending = false;
  bool _accessPending = false;

  bool get _active => _busy || const ['STARTING', 'RUNNING', 'STOPPING'].contains(_state);
  bool get _ready => _input['ready'] == true;

  @override
  void initState() {
    super.initState();
    _channel.setMethodCallHandler((call) async {
      if (!mounted) return;
      if (call.method == 'hostState') {
        _hostState(Map<Object?, Object?>.from(call.arguments as Map));
      } else if (call.method == 'inputState') {
        _inputState(Map<Object?, Object?>.from(call.arguments as Map));
      }
    });
    _load();
  }

  Future<void> _load() async {
    try {
      final state = await _channel.invokeMapMethod<Object?, Object?>('getHostState');
      if (!mounted) return;
      if (state != null) {
        _hostState(state);
        final input = state['input'];
        if (input is Map) _inputState(Map<Object?, Object?>.from(input));
      }
      final address = await _channel.invokeMethod<String>('address');
      if (mounted) setState(() => _address = address ?? '');
    } on PlatformException catch (e) {
      if (mounted) setState(() => _error = e.message ?? e.code);
    }
  }

  void _hostState(Map<Object?, Object?> state) {
    setState(() {
      _state = state['state'] as String? ?? 'IDLE';
      _reason = state['reason'] as String? ?? '';
      _audioAvailable = state['audioAvailable'] == true;
      _busy = state['busy'] == true;
    });
  }

  void _inputState(Map<Object?, Object?> input) {
    setState(() {
      _input = input;
      _mode = input['mode'] as int? ?? 2;
    });
  }

  Future<void> _invoke(String method, [Object? arguments]) async {
    setState(() => _error = '');
    try {
      await _channel.invokeMethod<Object?>(method, arguments);
    } on PlatformException catch (e) {
      if (mounted) setState(() => _error = e.message ?? e.code);
    }
  }

  Future<void> _start() async {
    setState(() => _startPending = true);
    await _invoke('start');
    if (mounted) setState(() => _startPending = false);
  }

  Future<void> _requestAccess() async {
    setState(() => _accessPending = true);
    await _invoke('requestInputAccess');
    if (mounted) setState(() => _accessPending = false);
  }

  @override
  void dispose() {
    _channel.setMethodCallHandler(null);
    _pin.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final features = <String>[
      if (_ready) 'Keyboard, mouse and touch',
      if (_input['relativeMouse'] == true) 'Genuine relative mouse',
      if (_input['multitouch'] == true) 'Multi-touch',
      if (_input['pen'] == true) 'Pen',
      if (_input['mappedText'] == true) 'Mapped text (not arbitrary Unicode)',
    ];
    final controllers = _input['maxControllers'] as int? ?? 0;
    return Scaffold(
      appBar: AppBar(title: const Text('Sunshine')),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          Text('Server address: ${_address.isEmpty ? 'Unavailable' : _address}'),
          const SizedBox(height: 16),
          Text('Host: $_state', style: Theme.of(context).textTheme.headlineSmall),
          if (_reason.isNotEmpty) Text(_reason),
          if (_error.isNotEmpty) Text(_error, style: TextStyle(color: Theme.of(context).colorScheme.error)),
          Text(_audioAvailable
              ? 'Playback audio capture available (subject to app capture policy)'
              : 'Video-only: playback capture needs Android 10+ and audio permission'),
          const SizedBox(height: 16),
          DropdownButtonFormField<int>(
            value: _mode,
            decoration: const InputDecoration(labelText: 'Control mode'),
            items: const [
              DropdownMenuItem(value: 2, child: Text('Shizuku')),
              DropdownMenuItem(value: 1, child: Text('Root (independent of Shizuku)')),
            ],
            onChanged: _active || _startPending ? null : (mode) {
              if (mode != null) _invoke('setInputMode', mode);
            },
          ),
          const SizedBox(height: 8),
          Text(_ready ? 'Controls ready' : 'Controls not ready'),
          if ((_input['uid'] as int? ?? -1) >= 0)
            Text('Worker UID: ${_input['uid']}'),
          if (features.isNotEmpty) Text(features.join(' · ')),
          Text(controllers > 0 ? 'Controllers available: $controllers' : 'Controllers unavailable on this backend/device'),
          if ((_input['failureReason'] as String? ?? '').isNotEmpty)
            Text(_input['failureReason'] as String),
          Align(
            alignment: Alignment.centerLeft,
            child: TextButton(
              onPressed: _active || _startPending || _accessPending ? null : _requestAccess,
              child: Text(_accessPending ? 'Requesting access…' : 'Request control access'),
            ),
          ),
          const SizedBox(height: 16),
          Wrap(spacing: 12, children: [
            ElevatedButton(
              onPressed: _ready && !_active && !_startPending ? _start : null,
              child: Text(_startPending ? 'Starting…' : 'Start Server'),
            ),
            OutlinedButton(
              onPressed: (_active || _startPending) && _state != 'STOPPING' ? () => _invoke('stop') : null,
              child: const Text('Stop Server'),
            ),
          ]),
          const SizedBox(height: 24),
          const Text('Pair Moonlight with this address, then enter its PIN below.'),
          const SizedBox(height: 8),
          TextField(
            controller: _pin,
            keyboardType: TextInputType.number,
            decoration: const InputDecoration(border: OutlineInputBorder(), labelText: 'PIN'),
          ),
          Align(
            alignment: Alignment.centerLeft,
            child: TextButton(
              onPressed: _state == 'RUNNING' ? () => _invoke('pin', _pin.text) : null,
              child: const Text('Add PIN'),
            ),
          ),
        ],
      ),
    );
  }
}
