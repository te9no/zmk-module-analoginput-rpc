import { useContext, useMemo, useState } from "react";
import "./App.css";
import { connect as serial_connect } from "@zmkfirmware/zmk-studio-ts-client/transport/serial";
import {
  ZMKConnection,
  ZMKCustomSubsystem,
  ZMKAppContext,
} from "@cormoran/zmk-studio-react-hook";
import {
  AnalogResponseCurve,
  AnalogRole,
  Request,
  Response,
  type AnalogAxisConfig,
  type AnalogInputDevice,
} from "./proto/dya/analog_input/analog_input";

const SUBSYSTEM_CANDIDATES = ["dya__studio", "dya_analog_input", "dya__analog_input", "analog_input", "analoginput"];
export const SUBSYSTEM_IDENTIFIER = "dya__studio";

function createDemoDevice(): AnalogInputDevice {
  return {
    id: 0,
    name: "Demo Analog Device",
    samplingHz: 125,
    reportIntervalMs: 8,
    axes: [
      {
        axisIndex: 0,
        name: "X Axis",
        enabled: true,
        adcChannel: 1,
        mvMid: 1800,
        mvMinMax: 1300,
        mvDeadzone: 40,
        scaleMultiplier: 1,
        scaleDivisor: 1,
        invert: false,
        role: AnalogRole.ANALOG_ROLE_REL_X,
        reportOnChangeOnly: true,
        outputMin: 0,
        outputMax: 30,
        responseCurve: AnalogResponseCurve.ANALOG_RESPONSE_CURVE_LINEAR,
      },
      {
        axisIndex: 1,
        name: "Y Axis",
        enabled: true,
        adcChannel: 2,
        mvMid: 1800,
        mvMinMax: 1300,
        mvDeadzone: 40,
        scaleMultiplier: 1,
        scaleDivisor: 1,
        invert: false,
        role: AnalogRole.ANALOG_ROLE_REL_Y,
        reportOnChangeOnly: true,
        outputMin: 0,
        outputMax: 30,
        responseCurve: AnalogResponseCurve.ANALOG_RESPONSE_CURVE_LINEAR,
      },
    ],
  };
}

function App() {
  const [demoMode, setDemoMode] = useState(false);

  return (
    <div className="app">
      <header className="app-header">
        <h1>DYA Analog Input Config</h1>
        <p>ZMK Studio RPC Web UI</p>
        <div className="row">
          <button className="btn btn-secondary" onClick={() => setDemoMode((v) => !v)}>
            {demoMode ? "Disable Demo Mode" : "Enable Demo Mode"}
          </button>
          {demoMode && <span>Demo mode active (no device required)</span>}
        </div>
      </header>

      <ZMKConnection
        renderDisconnected={({ connect, isLoading, error }) => (
          <>
            <section className="card">
              <h2>Device Connection</h2>
              {isLoading && <p>Connecting...</p>}
              {error && (
                <div className="error-message">
                  <p>{error}</p>
                </div>
              )}
              {!isLoading && (
                <button className="btn btn-primary" onClick={() => connect(serial_connect)}>
                  Connect Serial
                </button>
              )}
            </section>
            {demoMode && <RPCTestSection demoMode />}
          </>
        )}
        renderConnected={({ disconnect, deviceName }) => (
          <>
            <section className="card">
              <h2>Device Connection</h2>
              <div className="device-info">
                <h3>Connected to: {deviceName}</h3>
              </div>
              <button className="btn btn-secondary" onClick={disconnect}>
                Disconnect
              </button>
            </section>

            <RPCTestSection demoMode={demoMode} />
          </>
        )}
      />
    </div>
  );
}

export function RPCTestSection({ demoMode = false }: { demoMode?: boolean }) {
  const zmkApp = useContext(ZMKAppContext);
  const [devices, setDevices] = useState<AnalogInputDevice[]>(demoMode ? [createDemoDevice()] : []);
  const [selectedDeviceId, setSelectedDeviceId] = useState<number>(0);
  const [status, setStatus] = useState<string | null>(demoMode ? "Demo data loaded" : null);
  const [isLoading, setIsLoading] = useState(false);
  const [polling, setPolling] = useState(false);

  const subsystem = useMemo(() => {
    if (!zmkApp || demoMode) return null;

    for (const id of SUBSYSTEM_CANDIDATES) {
      const found = zmkApp.findSubsystem(id);
      if (found) return found;
    }

    const available = (zmkApp.state.connection as any)?.subsystems ?? [];
    const bestEffort = available.find((s: any) => {
      const id = String(s?.identifier ?? "").toLowerCase();
      return id.includes("analog") || id.includes("dya");
    });

    return bestEffort ?? null;
  }, [zmkApp, demoMode]);

  const callRPC = async (request: Request) => {
    if (!zmkApp?.state.connection || !subsystem) return null;
    const service = new ZMKCustomSubsystem(zmkApp.state.connection, subsystem.index);
    const payload = Request.encode(Request.create(request)).finish();
    const responsePayload = await service.callRPC(payload);
    if (!responsePayload) return null;
    const resp = Response.decode(responsePayload);
    if (resp.error) throw new Error(resp.error.message || "RPC error");
    return resp;
  };

  const refreshDevices = async () => {
    if (demoMode) {
      setDevices((current) => (current.length > 0 ? current : [createDemoDevice()]));
      setStatus("Demo devices refreshed");
      return;
    }

    setIsLoading(true);
    setStatus(null);
    try {
      const resp = await callRPC({ listDevices: {} });
      const list = resp?.listDevices?.devices ?? [];
      setDevices(list);
      if (list.length > 0 && !list.some((d) => d.id === selectedDeviceId)) setSelectedDeviceId(list[0].id);
      setStatus(`Loaded ${list.length} device(s)`);
    } catch (error) {
      setStatus(error instanceof Error ? error.message : "Failed to load devices");
    } finally {
      setIsLoading(false);
    }
  };

  const selectedDevice = devices.find((d) => d.id === selectedDeviceId) ?? null;

  const replaceSelectedDevice = (updater: (device: AnalogInputDevice) => AnalogInputDevice) => {
    setDevices((current) =>
      current.map((d) => (d.id === selectedDeviceId ? updater(d) : d))
    );
  };

  const updateDeviceField = async (field: "sampling_hz" | "report_interval_ms", value: number) => {
    if (!selectedDevice) return;

    if (demoMode) {
      replaceSelectedDevice((d) => ({
        ...d,
        samplingHz: field === "sampling_hz" ? value : d.samplingHz,
        reportIntervalMs: field === "report_interval_ms" ? value : d.reportIntervalMs,
      }));
      setStatus("Demo settings updated");
      return;
    }

    setIsLoading(true);
    try {
      if (field === "sampling_hz") {
        await callRPC({ setSamplingHz: { id: selectedDevice.id, value } });
      } else {
        await callRPC({ setReportInterval: { id: selectedDevice.id, valueMs: value } });
      }
      await refreshDevices();
    } finally {
      setIsLoading(false);
    }
  };

  const updateAxis = async (axis: AnalogAxisConfig) => {
    if (!selectedDevice) return;

    if (demoMode) {
      replaceSelectedDevice((d) => ({
        ...d,
        axes: d.axes.map((a) => (a.axisIndex === axis.axisIndex ? axis : a)),
      }));
      setStatus(`Demo axis ${axis.axisIndex} updated`);
      return;
    }

    setIsLoading(true);
    try {
      await callRPC({ setAxisConfig: { deviceId: selectedDevice.id, axis } });
      await refreshDevices();
    } finally {
      setIsLoading(false);
    }
  };

  const resetDevice = async () => {
    if (!selectedDevice) return;

    if (demoMode) {
      setDevices([createDemoDevice()]);
      setSelectedDeviceId(0);
      setStatus("Demo device reset to defaults");
      return;
    }

    setIsLoading(true);
    try {
      await callRPC({ resetDevice: { id: selectedDevice.id } });
      await refreshDevices();
    } finally {
      setIsLoading(false);
    }
  };

  const loadValues = async () => {
    if (!selectedDevice) return;

    if (demoMode) {
      const make = () => Math.floor(Math.random() * 4000);
      const lines = selectedDevice.axes.map((a) => {
        const raw = make();
        const mv = raw;
        const out = Math.floor((raw / 4000) * a.outputMax);
        return `axis${a.axisIndex}=raw:${raw},mv:${mv},out:${out}`;
      });
      setStatus(`Demo values: ${lines.join(" / ")}`);
      return;
    }

    setPolling(true);
    try {
      const resp = await callRPC({ getValues: { id: selectedDevice.id } });
      const values = resp?.getValues?.values ?? [];
      setStatus(
        `Live values: ${values
          .map((v) => `axis${v.axisIndex}=raw:${v.raw},mv:${v.mv},out:${v.reportValue}`)
          .join(" / ")}`
      );
    } finally {
      setPolling(false);
    }
  };

  if (!demoMode && !zmkApp) return null;

  if (!demoMode && !subsystem) {
    return (
      <section className="card">
        <div className="warning-message">
          <p>AnalogInput subsystem not found. Tried: {SUBSYSTEM_CANDIDATES.join(", ")}</p>
        </div>
      </section>
    );
  }

  return (
    <section className="card">
      <h2>Analog Input Settings {demoMode ? "(Demo)" : ""}</h2>
      <p>Configure all runtime settings via {demoMode ? "local demo state" : "RPC"}.</p>
      <div className="row">
        <button className="btn btn-primary" disabled={isLoading} onClick={refreshDevices}>
          Refresh Devices
        </button>
        {status && <span>{status}</span>}
      </div>

      {devices.length > 0 && (
        <div className="input-group">
          <label htmlFor="device-select">Device</label>
          <select id="device-select" value={selectedDeviceId} onChange={(e) => setSelectedDeviceId(Number(e.target.value))}>
            {devices.map((d) => (
              <option key={d.id} value={d.id}>
                {d.id}: {d.name}
              </option>
            ))}
          </select>
        </div>
      )}

      {selectedDevice && (
        <div className="axis-panel">
          <h3>Device Settings</h3>
          <div className="input-group">
            <label htmlFor="sampling_hz">sampling_hz</label>
            <input id="sampling_hz" type="number" value={selectedDevice.samplingHz} onChange={(e) => updateDeviceField("sampling_hz", Number(e.target.value) || 0)} />
          </div>
          <div className="input-group">
            <label htmlFor="report_interval_ms">report_interval_ms</label>
            <input id="report_interval_ms" type="number" value={selectedDevice.reportIntervalMs} onChange={(e) => updateDeviceField("report_interval_ms", Number(e.target.value) || 0)} />
          </div>
          <div className="row">
            <button className="btn btn-secondary" onClick={loadValues} disabled={polling}>
              {polling ? "Loading values..." : "Read Values"}
            </button>
            <button className="btn btn-secondary" onClick={resetDevice}>Reset Device</button>
          </div>
          {selectedDevice.axes.map((axis) => (
            <AxisEditor key={axis.axisIndex} axis={axis} onUpdate={updateAxis} />
          ))}
        </div>
      )}
    </section>
  );
}

function AxisEditor({ axis, onUpdate }: { axis: AnalogAxisConfig; onUpdate: (axis: AnalogAxisConfig) => Promise<void> }) {
  const [draft, setDraft] = useState<AnalogAxisConfig>(axis);

  const set = <K extends keyof AnalogAxisConfig>(key: K, value: AnalogAxisConfig[K]) => {
    setDraft((prev) => ({ ...prev, [key]: value }));
  };

  return (
    <fieldset className="axis-box">
      <legend>Axis {axis.axisIndex}</legend>
      <div className="input-group"><label>Name</label><input value={draft.name} onChange={(e) => set("name", e.target.value)} /></div>
      <div className="input-group"><label>enabled</label><input type="checkbox" checked={draft.enabled} onChange={(e) => set("enabled", e.target.checked)} /></div>
      <div className="input-group"><label>adc_channel</label><input type="number" value={draft.adcChannel} onChange={(e) => set("adcChannel", Number(e.target.value) || 0)} /></div>
      <div className="input-group"><label>mv_mid</label><input type="number" value={draft.mvMid} onChange={(e) => set("mvMid", Number(e.target.value) || 0)} /></div>
      <div className="input-group"><label>mv_min_max</label><input type="number" value={draft.mvMinMax} onChange={(e) => set("mvMinMax", Number(e.target.value) || 0)} /></div>
      <div className="input-group"><label>mv_deadzone</label><input type="number" value={draft.mvDeadzone} onChange={(e) => set("mvDeadzone", Number(e.target.value) || 0)} /></div>
      <div className="input-group"><label>scale_multiplier</label><input type="number" value={draft.scaleMultiplier} onChange={(e) => set("scaleMultiplier", Number(e.target.value) || 1)} /></div>
      <div className="input-group"><label>scale_divisor</label><input type="number" value={draft.scaleDivisor} onChange={(e) => set("scaleDivisor", Number(e.target.value) || 1)} /></div>
      <div className="input-group"><label>invert</label><input type="checkbox" checked={draft.invert} onChange={(e) => set("invert", e.target.checked)} /></div>
      <div className="input-group"><label>report_on_change_only</label><input type="checkbox" checked={draft.reportOnChangeOnly} onChange={(e) => set("reportOnChangeOnly", e.target.checked)} /></div>
      <div className="input-group"><label>output_min</label><input type="number" value={draft.outputMin} onChange={(e) => set("outputMin", Number(e.target.value) || 0)} /></div>
      <div className="input-group"><label>output_max</label><input type="number" value={draft.outputMax} onChange={(e) => set("outputMax", Number(e.target.value) || 0)} /></div>
      <div className="input-group">
        <label>role</label>
        <select value={draft.role} onChange={(e) => set("role", Number(e.target.value) as AnalogRole)}>
          {Object.entries(AnalogRole).filter(([, v]) => typeof v === "number").map(([k, v]) => <option key={k} value={v as number}>{k}</option>)}
        </select>
      </div>
      <div className="input-group">
        <label>response_curve</label>
        <select value={draft.responseCurve} onChange={(e) => set("responseCurve", Number(e.target.value) as AnalogResponseCurve)}>
          {Object.entries(AnalogResponseCurve).filter(([, v]) => typeof v === "number").map(([k, v]) => <option key={k} value={v as number}>{k}</option>)}
        </select>
      </div>
      <button className="btn btn-primary" onClick={() => onUpdate(draft)}>Apply Axis</button>
    </fieldset>
  );
}

export default App;
