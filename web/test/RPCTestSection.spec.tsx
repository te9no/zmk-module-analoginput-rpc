import { render, screen } from "@testing-library/react";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import { RPCTestSection, SUBSYSTEM_IDENTIFIER } from "../src/App";

describe("RPCTestSection", () => {
  it("renders analog settings section when subsystem exists", () => {
    const mockZMKApp = createConnectedMockZMKApp({
      deviceName: "Test Device",
      subsystems: [SUBSYSTEM_IDENTIFIER],
    });

    render(
      <ZMKAppProvider value={mockZMKApp}>
        <RPCTestSection />
      </ZMKAppProvider>
    );

    expect(screen.getByText(/Analog Input Settings/i)).toBeInTheDocument();
    expect(screen.getByText(/Refresh Devices/i)).toBeInTheDocument();
  });

  it("shows warning when subsystem is not found", () => {
    const mockZMKApp = createConnectedMockZMKApp({
      subsystems: [],
    });

    render(
      <ZMKAppProvider value={mockZMKApp}>
        <RPCTestSection />
      </ZMKAppProvider>
    );

    expect(screen.getByText(/AnalogInput subsystem not found/i)).toBeInTheDocument();
  });
});
