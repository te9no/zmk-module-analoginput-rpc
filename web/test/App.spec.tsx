import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { setupZMKMocks } from "@cormoran/zmk-studio-react-hook/testing";
import App, { SUBSYSTEM_IDENTIFIER } from "../src/App";

jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({
  create_rpc_connection: jest.fn(),
  call_rpc: jest.fn(),
}));

jest.mock("@zmkfirmware/zmk-studio-ts-client/transport/serial", () => ({
  connect: jest.fn(),
}));

describe("App Component", () => {
  it("renders header and connect button", () => {
    render(<App />);
    expect(screen.getByText(/DYA Analog Input Config/i)).toBeInTheDocument();
    expect(screen.getByText(/Connect Serial/i)).toBeInTheDocument();
  });

  it("connects to device", async () => {
    const mocks = setupZMKMocks();
    mocks.mockSuccessfulConnection({ deviceName: "Test Keyboard", subsystems: [SUBSYSTEM_IDENTIFIER] });

    const { connect: serialConnect } = await import("@zmkfirmware/zmk-studio-ts-client/transport/serial");
    (serialConnect as jest.Mock).mockResolvedValue(mocks.mockTransport);

    render(<App />);
    const user = userEvent.setup();
    await user.click(screen.getByText(/Connect Serial/i));

    await waitFor(() => {
      expect(screen.getByText(/Connected to: Test Keyboard/i)).toBeInTheDocument();
    });
  });
});
