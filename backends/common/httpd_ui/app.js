/* oveRTOS dashboard — Alpine.js application logic */

async function api(method, path, body) {
	var opts = {method: method};
	if (body) {
		opts.headers = {'Content-Type': 'application/json'};
		opts.body = JSON.stringify(body);
	}
	try {
		var r = await fetch(path, opts);
		return await r.json();
	} catch (e) {
		return null;
	}
}

function fmtUptime(s) {
	var h = Math.floor(s / 3600);
	var m = Math.floor((s % 3600) / 60);
	var sc = s % 60;
	return h + ':' + String(m).padStart(2, '0') + ':' + String(sc).padStart(2, '0');
}

/* Alpine data component for the entire dashboard */
function dashboard() {
	return {
		/* system info */
		board: '', rtos: '', uptime: 0, ip: '', version: '',

		/* memory */
		mem: {total: 0, free: 0, used: 0, peak: 0},

		/* threads */
		threads: [],

		/* audio (conditional) */
		audio_enabled: false,
		audio: {cycles: 0, underruns: 0, overruns: 0, max_us: 0, avg_us: 0},

		/* inference (conditional) */
		infer_enabled: false,
		infer_us: 0,

		/* LEDs */
		leds: [],

		/* GPIO */
		gpio: [
			{port: 0, pin: 0, value: '-'},
			{port: 0, pin: 1, value: '-'},
			{port: 0, pin: 2, value: '-'},
			{port: 0, pin: 3, value: '-'}
		],

		/* network */
		net_ip: '', net_gw: '', net_dns: '',
		net_current: '',

		/* log console */
		logBuf: '',

		/* shell terminal */
		shell_enabled: false,
		termBuf: '',
		cmdInput: '',
		_ws_shell: null,

		/* toast */
		toast: '', toastVisible: false,

		/* polling timers */
		_timers: [],

		showToast(msg) {
			this.toast = msg;
			this.toastVisible = true;
			setTimeout(() => { this.toastVisible = false; }, 3000);
		},

		async loadInfo() {
			var d = await api('GET', '/api/info');
			if (!d) return;
			this.board = d.board;
			this.rtos = d.rtos;
			this.uptime = d.uptime;
			this.ip = d.ip;
			this.version = d.version || '';
		},

		async loadMemory() {
			var d = await api('GET', '/api/system/memory');
			if (!d) return;
			this.mem = {total: d.total, free: d.free, used: d.used, peak: d.peak || 0};
		},

		async loadThreads() {
			var d = await api('GET', '/api/system/threads');
			if (!d || !d.threads) return;
			this.threads = d.threads;
		},

		async loadAudioStats() {
			var d = await api('GET', '/api/audio/stats');
			if (!d) return;
			if (d.error) { this.audio_enabled = false; return; }
			this.audio_enabled = true;
			this.audio = d;
		},

		async loadInferStats() {
			var d = await api('GET', '/api/infer/stats');
			if (!d) return;
			if (d.error) { this.infer_enabled = false; return; }
			this.infer_enabled = true;
			this.infer_us = d.last_inference_us;
		},

		async loadLeds() {
			var d = await api('GET', '/api/leds');
			if (!d || !d.leds) return;
			this.leds = d.leds;
		},

		async toggleLed(id, currentState) {
			var d = await api('POST', '/api/leds/' + id, {on: !currentState});
			if (!d) { this.showToast('LED toggle failed'); return; }
			/* optimistic update */
			var led = this.leds.find(function(l) { return l.id === id; });
			if (led) led.on = !currentState;
		},

		async readGpio(idx) {
			var g = this.gpio[idx];
			var d = await api('GET', '/api/gpio/' + g.port + '/' + g.pin);
			if (!d) { this.showToast('GPIO read failed'); return; }
			this.gpio[idx].value = d.value;
		},

		async toggleGpio(idx) {
			var g = this.gpio[idx];
			await api('POST', '/api/gpio/' + g.port + '/' + g.pin, {toggle: true});
			this.readGpio(idx);
		},

		async loadNetwork() {
			var d = await api('GET', '/api/network');
			if (!d) return;
			this.net_ip = d.ip || '';
			this.net_gw = d.gateway || '';
			this.net_dns = d.dns || '';
			this.net_current = d.ip + ' / ' + d.gateway + ' / ' + d.dns;
		},

		async saveNetwork() {
			var d = await api('POST', '/api/network', {
				ip: this.net_ip, gateway: this.net_gw, dns: this.net_dns
			});
			if (d) this.showToast('Network settings saved');
			else this.showToast('Save failed');
		},

		async loadLog() {
			var d = await api('GET', '/api/log');
			if (!d || !d.lines) return;
			this.logBuf += d.lines.join('\n') + '\n';
			this.$nextTick(() => {
				var el = this.$refs.console;
				if (el) el.scrollTop = el.scrollHeight;
			});
		},

		clearLog() {
			this.logBuf = '';
		},

		connectShellWs() {
			var proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
			var ws = new WebSocket(proto + '//' + location.host + '/ws/shell');
			ws.onopen = () => { this.shell_enabled = true; };
			ws.onmessage = (e) => {
				this.termBuf += e.data;
				this.$nextTick(() => {
					var el = this.$refs.terminal;
					if (el) el.scrollTop = el.scrollHeight;
				});
			};
			ws.onclose = () => { this.shell_enabled = false; };
			ws.onerror = () => { this.shell_enabled = false; };
			this._ws_shell = ws;
		},

		sendCmd() {
			if (this._ws_shell && this._ws_shell.readyState === 1 && this.cmdInput) {
				this._ws_shell.send(this.cmdInput);
				this.termBuf += '> ' + this.cmdInput + '\n';
				this.cmdInput = '';
			}
		},

		init() {
			this.loadInfo();
			this.loadLeds();
			this.loadMemory();
			this.loadThreads();
			this.loadNetwork();
			this.loadLog();
			this._timers.push(setInterval(() => this.loadInfo(), 2000));
			this._timers.push(setInterval(() => this.loadMemory(), 2000));
			this._timers.push(setInterval(() => this.loadThreads(), 2000));
			this.loadAudioStats();
			this.loadInferStats();
			this.connectShellWs();
			this._timers.push(setInterval(() => {
				if (this.audio_enabled) this.loadAudioStats();
			}, 2000));
			this._timers.push(setInterval(() => {
				if (this.infer_enabled) this.loadInferStats();
			}, 2000));
			this._timers.push(setInterval(() => this.loadLog(), 1000));
		},

		destroy() {
			this._timers.forEach(clearInterval);
		}
	};
}
