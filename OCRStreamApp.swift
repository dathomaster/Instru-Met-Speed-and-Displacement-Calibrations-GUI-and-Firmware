//
//  OCRStreamApp.swift
//  iPhone Numeric OCR Streamer
//
//  Drop this single file into a fresh iOS App project (SwiftUI).
//  Requires iOS 16+ for VNRecognizeTextRequest revision 3 numeric recognition.
//
//  Info.plist keys required:
//    - NSCameraUsageDescription          (e.g. "Used to OCR numbers from the camera")
//    - NSLocalNetworkUsageDescription    (e.g. "Streams OCR readings to your computer")
//    - NSLocationWhenInUseUsageDescription (e.g. "Sends GPS coordinates with OCR stream")
//
//  Usage: enter your Mac/PC's LAN IP, tap Stream. Point at numbers.
//  The yellow box is the region of interest — only numbers inside it are read.
//  Use the "Box" slider to size it around your instrument's display.
//

import SwiftUI
import AVFoundation
import Vision
import Network
import Combine
import CoreLocation

// MARK: - App entry point
@main
struct OCRStreamApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
        }
    }
}

// MARK: - Main view
struct ContentView: View {
    @StateObject private var ocr = OCRStreamer()
    @AppStorage("hostIP") private var hostIP: String = "192.168.1.100"
    @AppStorage("hostPort") private var hostPort: String = "9999"
    @AppStorage("roiW") private var roiW: Double = 0.35        // box width, fraction of frame
    @AppStorage("roiH") private var roiH: Double = 0.05        // box height, fraction of frame

    var body: some View {
        ZStack {
            // Camera preview fills the screen
            CameraPreview(session: ocr.session)
                .ignoresSafeArea()

            // ROI box + dimmed surround. Centered, with adjustable width & height.
            GeometryReader { geo in
                let boxW = geo.size.width * roiW
                let boxH = geo.size.height * roiH
                let boxRect = CGRect(x: (geo.size.width - boxW) / 2,
                                     y: (geo.size.height - boxH) / 2,
                                     width: boxW, height: boxH)
                ZStack {
                    // Dim everything outside the ROI so it's clear what's being read
                    Path { p in
                        p.addRect(CGRect(origin: .zero, size: geo.size))
                        p.addRect(boxRect)
                    }
                    .fill(Color.black.opacity(0.45), style: FillStyle(eoFill: true))

                    // ROI outline
                    Rectangle()
                        .stroke(Color.yellow, lineWidth: 2)
                        .frame(width: boxW, height: boxH)
                        .position(x: geo.size.width / 2, y: geo.size.height / 2)
                }
            }
            .allowsHitTesting(false)
            .ignoresSafeArea()

            // Bounding boxes for what's being read (these are now only from inside the ROI)
            GeometryReader { geo in
                ForEach(ocr.detections) { d in
                    Rectangle()
                        .stroke(Color.green, lineWidth: 2)
                        .frame(width: d.rect.width * geo.size.width,
                               height: d.rect.height * geo.size.height)
                        .position(x: d.rect.midX * geo.size.width,
                                  y: (1 - d.rect.midY) * geo.size.height)
                    Text(d.text)
                        .font(.caption.bold())
                        .padding(4)
                        .background(Color.green.opacity(0.8))
                        .foregroundColor(.black)
                        .position(x: d.rect.midX * geo.size.width,
                                  y: (1 - d.rect.maxY) * geo.size.height - 12)
                }
            }
            .allowsHitTesting(false)

            VStack {
                Spacer()
                controlPanel
            }
        }
        .onAppear {
            ocr.setROI(width: roiW, height: roiH)
            ocr.start()
        }
        .onChange(of: roiW) { _, _ in ocr.setROI(width: roiW, height: roiH) }
        .onChange(of: roiH) { _, _ in ocr.setROI(width: roiW, height: roiH) }
    }

    private var controlPanel: some View {
        VStack(spacing: 10) {
            HStack {
                TextField("Host IP", text: $hostIP)
                    .textFieldStyle(.roundedBorder)
                    .keyboardType(.decimalPad)
                    .frame(maxWidth: 180)
                TextField("Port", text: $hostPort)
                    .textFieldStyle(.roundedBorder)
                    .keyboardType(.numberPad)
                    .frame(width: 80)
                Button(ocr.isStreaming ? "Stop" : "Stream") {
                    if ocr.isStreaming { ocr.stopStreaming() }
                    else { ocr.startStreaming(host: hostIP, port: UInt16(hostPort) ?? 9999) }
                }
                .buttonStyle(.borderedProminent)
            }

            HStack(spacing: 16) {
                Text(ocr.latestNumber ?? "—")
                    .font(.system(size: 36, weight: .bold, design: .monospaced))
                    .foregroundColor(.white)
                    .shadow(radius: 2)
                Spacer()
                VStack(alignment: .trailing) {
                    Text("OCR: \(ocr.ocrHz, specifier: "%.1f") Hz")
                    Text("Sent: \(ocr.sentCount)")
                }
                .font(.caption.monospaced())
                .foregroundColor(.white)
                .shadow(radius: 2)
            }

            HStack(spacing: 10) {
                Image(systemName: "arrow.left.and.right")
                    .foregroundColor(.yellow)
                Text("Width")
                    .font(.caption)
                    .foregroundColor(.white)
                Slider(value: $roiW, in: 0.10...0.95)
                    .tint(.yellow)
                Text("\(Int(roiW * 100))%")
                    .font(.caption.monospaced())
                    .foregroundColor(.white)
                    .frame(width: 38, alignment: .trailing)
            }

            HStack(spacing: 10) {
                Image(systemName: "arrow.up.and.down")
                    .foregroundColor(.yellow)
                Text("Height")
                    .font(.caption)
                    .foregroundColor(.white)
                Slider(value: $roiH, in: 0.05...0.95)
                    .tint(.yellow)
                Text("\(Int(roiH * 100))%")
                    .font(.caption.monospaced())
                    .foregroundColor(.white)
                    .frame(width: 38, alignment: .trailing)
                Button("Default") {
                    roiW = 0.35; roiH = 0.05
                }
                .font(.caption)
                .foregroundColor(.yellow)
            }

            // GPS status
            if let loc = ocr.lastLocation {
                Text(String(format: "GPS %.5f, %.5f  alt %.1f m",
                            loc.coordinate.latitude, loc.coordinate.longitude, loc.altitude))
                    .font(.system(size: 10, design: .monospaced))
                    .foregroundColor(.white.opacity(0.6))
            }
        }
        .padding()
        .background(.black.opacity(0.55))
        .padding(.horizontal, 8)
        .padding(.bottom, 8)
    }
}

// MARK: - Detection model
struct Detection: Identifiable {
    let id = UUID()
    let text: String
    let rect: CGRect  // normalized 0..1, origin bottom-left (Vision convention)
}

// MARK: - OCR + streaming engine
final class OCRStreamer: NSObject, ObservableObject, AVCaptureVideoDataOutputSampleBufferDelegate {

    // Published UI state
    @Published var detections: [Detection] = []
    @Published var latestNumber: String?
    @Published var isStreaming = false
    @Published var ocrHz: Double = 0
    @Published var sentCount: Int = 0
    @Published var lastLocation: CLLocation?

    // Location — fix requested on stream start, re-sent every 3 s while streaming
    private let locationManager = CLLocationManager()
    private var locationDelegate: LocationDelegate?
    private var gpsTimer: Timer?

    // Region of interest: a centered box with adjustable width & height
    // (fractions of the frame). Vision only OCRs inside this, so other numbers
    // in view are ignored. Accessed from main thread (setter) and vision thread.
    private var _roiW: Double = 0.50
    private var _roiH: Double = 0.30
    private let roiLock = NSLock()

    func setROI(width: Double, height: Double) {
        roiLock.lock()
        _roiW = min(0.98, max(0.05, width))
        _roiH = min(0.98, max(0.05, height))
        roiLock.unlock()
    }

    /// Vision regionOfInterest rect (normalized, origin bottom-left, in the
    /// buffer's pre-orientation space) for the current centered box.
    /// Under .right orientation the buffer is rotated 90°, so the on-screen
    /// width and height swap axes in the buffer. The box stays centered, so
    /// a centered rect maps to a centered rect — only the dimensions swap.
    private func currentVisionROI() -> CGRect {
        roiLock.lock()
        let w = _roiW, h = _roiH
        roiLock.unlock()
        // Screen width  → buffer height ; screen height → buffer width
        let bufW = h
        let bufH = w
        let originX = (1.0 - bufW) / 2.0
        let originY = (1.0 - bufH) / 2.0
        return CGRect(x: originX, y: originY, width: bufW, height: bufH)
    }

    // Capture
    let session = AVCaptureSession()
    private let videoQueue = DispatchQueue(label: "ocr.video.queue", qos: .userInteractive)
    private let videoOutput = AVCaptureVideoDataOutput()

    // Vision
    private let textRequest: VNRecognizeTextRequest
    private let visionQueue = DispatchQueue(label: "ocr.vision.queue", qos: .userInteractive)
    private var inFlight = false  // drop frames if Vision is busy → maximum throughput

    // Networking
    private var connection: NWConnection?
    private let netQueue = DispatchQueue(label: "ocr.net.queue", qos: .userInteractive)

    // Hz measurement
    private var frameTimes: [CFAbsoluteTime] = []

    // Regex: signed decimal numbers like -0.1235, 1200.0, 003.2, 1, .5, -.5
    private static let numberPattern: NSRegularExpression = {
        // Optional sign, then either: digits with optional fractional part, or .digits
        try! NSRegularExpression(pattern: #"-?(?:\d+(?:\.\d+)?|\.\d+)"#)
    }()

    override init() {
        // Configure Vision request once, reuse forever
        self.textRequest = VNRecognizeTextRequest()
        textRequest.recognitionLevel = .accurate          // ← Apple's accurate OCR, as requested
        textRequest.usesLanguageCorrection = false        // numbers don't benefit from language model
        textRequest.recognitionLanguages = ["en-US"]
        textRequest.revision = VNRecognizeTextRequestRevision3
        // Bias the recognizer toward numeric glyphs:
        textRequest.customWords = ["0","1","2","3","4","5","6","7","8","9",".","-"]
        textRequest.minimumTextHeight = 0.02              // ignore tiny noise

        super.init()
        configureSession()
    }

    // MARK: Camera setup
    private func configureSession() {
        session.beginConfiguration()
        session.sessionPreset = .hd1920x1080

        guard let device = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back),
              let input = try? AVCaptureDeviceInput(device: device),
              session.canAddInput(input) else {
            session.commitConfiguration()
            return
        }
        session.addInput(input)

        videoOutput.alwaysDiscardsLateVideoFrames = true   // critical for low latency
        videoOutput.videoSettings = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA
        ]
        videoOutput.setSampleBufferDelegate(self, queue: videoQueue)
        if session.canAddOutput(videoOutput) { session.addOutput(videoOutput) }

        if let conn = videoOutput.connection(with: .video) {
            if conn.isVideoOrientationSupported { conn.videoOrientation = .portrait }
        }

        // Try to lock continuous autofocus on near subjects (helpful for screens/gauges)
        try? device.lockForConfiguration()
        if device.isFocusModeSupported(.continuousAutoFocus) { device.focusMode = .continuousAutoFocus }
        if device.isExposureModeSupported(.continuousAutoExposure) { device.exposureMode = .continuousAutoExposure }
        device.unlockForConfiguration()

        session.commitConfiguration()
    }

    func start() {
        AVCaptureDevice.requestAccess(for: .video) { [weak self] ok in
            guard ok, let self else { return }
            self.videoQueue.async { self.session.startRunning() }
        }
    }

    // MARK: Streaming control
    func startStreaming(host: String, port: UInt16) {
        stopStreaming()
        let endpoint = NWEndpoint.hostPort(host: NWEndpoint.Host(host),
                                           port: NWEndpoint.Port(rawValue: port)!)
        let conn = NWConnection(to: endpoint, using: .udp)
        conn.start(queue: netQueue)
        connection = conn
        DispatchQueue.main.async { self.isStreaming = true; self.sentCount = 0 }
        // Initial fix + start the 3 s repeat timer
        requestLocation()
        startGPSTimer()
    }

    func stopStreaming() {
        connection?.cancel()
        connection = nil
        stopGPSTimer()
        DispatchQueue.main.async { self.isStreaming = false }
    }

    private func startGPSTimer() {
        DispatchQueue.main.async {
            self.gpsTimer?.invalidate()
            // Re-request a fresh fix every 3 s and re-send the last known coord.
            self.gpsTimer = Timer.scheduledTimer(withTimeInterval: 3.0, repeats: true) { [weak self] _ in
                guard let self else { return }
                // Ask CoreLocation for a fresh fix (cheap, often returns cached)
                if self.locationManager.authorizationStatus == .authorizedWhenInUse
                   || self.locationManager.authorizationStatus == .authorizedAlways {
                    self.locationManager.requestLocation()
                }
                // Also re-send the last known coord in case the receiver was restarted
                if let loc = self.lastLocation {
                    let ts = Date().timeIntervalSince1970
                    let payload = String(format: "GPS\t%.6f\t%.8f\t%.8f\t%.3f\n",
                                         ts, loc.coordinate.latitude, loc.coordinate.longitude,
                                         loc.altitude)
                    self.send(payload)
                }
            }
        }
    }

    private func stopGPSTimer() {
        DispatchQueue.main.async {
            self.gpsTimer?.invalidate()
            self.gpsTimer = nil
        }
    }

    private func requestLocation() {
        // Set up the delegate once. Authorization is async — we must wait for the
        // didChangeAuthorization callback before requesting a fix, otherwise the
        // one-shot requestLocation() is dropped before permission is granted.
        if locationDelegate == nil {
            let delegate = LocationDelegate(
                onFix: { [weak self] loc in
                    guard let self else { return }
                    DispatchQueue.main.async { self.lastLocation = loc }
                    let ts = Date().timeIntervalSince1970
                    let payload = String(format: "GPS\t%.6f\t%.8f\t%.8f\t%.3f\n",
                                         ts, loc.coordinate.latitude, loc.coordinate.longitude,
                                         loc.altitude)
                    self.send(payload)
                },
                onAuthorized: { [weak self] in
                    // Now we have permission — safe to request the actual fix.
                    self?.locationManager.requestLocation()
                }
            )
            locationDelegate = delegate
            locationManager.delegate = delegate
            locationManager.desiredAccuracy = kCLLocationAccuracyBest
        }

        let status = locationManager.authorizationStatus
        switch status {
        case .notDetermined:
            // Triggers the permission prompt. requestLocation() is called later,
            // from the onAuthorized callback, once the user responds.
            locationManager.requestWhenInUseAuthorization()
        case .authorizedWhenInUse, .authorizedAlways:
            locationManager.requestLocation()
        default:
            break   // denied/restricted — GPS unavailable, OCR still works
        }
    }

    private func send(_ payload: String) {
        guard let conn = connection else { return }
        let data = payload.data(using: .utf8) ?? Data()
        conn.send(content: data, completion: .idempotent)
        DispatchQueue.main.async { self.sentCount += 1 }
    }

    // MARK: Frame callback
    func captureOutput(_ output: AVCaptureOutput,
                       didOutput sampleBuffer: CMSampleBuffer,
                       from connection: AVCaptureConnection) {
        // Drop frame if Vision is still processing the previous one — keeps us at max useful Hz
        guard !inFlight, let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        inFlight = true

        visionQueue.async { [weak self] in
            guard let self else { return }
            defer { self.inFlight = false }

            // Set the region of interest so Vision only reads inside the box.
            // currentVisionROI() maps the on-screen yellow box (width & height)
            // into the buffer's coordinate space for the .right orientation.
            self.textRequest.regionOfInterest = self.currentVisionROI()

            let handler = VNImageRequestHandler(cvPixelBuffer: pixelBuffer,
                                                orientation: .right, options: [:])
            do {
                try handler.perform([self.textRequest])
            } catch {
                return
            }
            self.handleResults(self.textRequest.results ?? [])
            self.tickHz()
        }
    }

    // MARK: Result handling
    private func handleResults(_ observations: [VNRecognizedTextObservation]) {
        var found: [Detection] = []
        var bestNumber: String?
        var bestArea: CGFloat = 0

        for obs in observations {
            // Take top candidate AND require minimum confidence — drops half-seen digits
            guard let cand = obs.topCandidates(1).first, cand.confidence >= 0.5 else { continue }
            let text = cand.string
            let ns = text as NSString
            let range = NSRange(location: 0, length: ns.length)
            let matches = OCRStreamer.numberPattern.matches(in: text, range: range)
            for m in matches {
                let numStr = ns.substring(with: m.range)
                guard isValidNumber(numStr) else { continue }

                // Anti-"missing decimal" heuristic: if the *original* recognized string had a
                // dot or comma anywhere in it but our extracted number doesn't, skip — we
                // probably grabbed only the integer half of a decimal. Better to drop the
                // frame than report e.g. "1359" when the display really says "0.1359".
                let originalHasSeparator = text.contains(".") || text.contains(",")
                let extractedHasDot = numStr.contains(".")
                if originalHasSeparator && !extractedHasDot { continue }

                found.append(Detection(text: numStr, rect: obs.boundingBox))
                // Pick the largest detection as the "primary" reading
                let area = obs.boundingBox.width * obs.boundingBox.height
                if area > bestArea {
                    bestArea = area
                    bestNumber = numStr
                }
            }
        }

        // Stream the primary number with a timestamp, newline-delimited (one reading per UDP packet)
        if let n = bestNumber {
            let ts = Date().timeIntervalSince1970
            let payload = String(format: "%.6f\t%@\n", ts, n)
            send(payload)
        }

        DispatchQueue.main.async {
            self.detections = found
            self.latestNumber = bestNumber ?? self.latestNumber
        }
    }

    private func isValidNumber(_ s: String) -> Bool {
        // Sanity: must parse as Double, must contain at least one digit, reject lone "-" or "."
        guard !s.isEmpty, s != "-", s != ".", s != "-." else { return false }
        return Double(s) != nil
    }

    private func tickHz() {
        let now = CFAbsoluteTimeGetCurrent()
        frameTimes.append(now)
        // Keep last ~1s window
        while let first = frameTimes.first, now - first > 1.0 { frameTimes.removeFirst() }
        let hz = Double(frameTimes.count)
        DispatchQueue.main.async { self.ocrHz = hz }
    }
}

// MARK: - Camera preview bridge
struct CameraPreview: UIViewRepresentable {
    let session: AVCaptureSession

    func makeUIView(context: Context) -> PreviewView {
        let v = PreviewView()
        v.videoPreviewLayer.session = session
        v.videoPreviewLayer.videoGravity = .resizeAspectFill
        if let conn = v.videoPreviewLayer.connection, conn.isVideoOrientationSupported {
            conn.videoOrientation = .portrait
        }
        return v
    }
    func updateUIView(_ uiView: PreviewView, context: Context) {}
}

final class PreviewView: UIView {
    override class var layerClass: AnyClass { AVCaptureVideoPreviewLayer.self }
    var videoPreviewLayer: AVCaptureVideoPreviewLayer { layer as! AVCaptureVideoPreviewLayer }
}

// MARK: - Location delegate
final class LocationDelegate: NSObject, CLLocationManagerDelegate {
    private let onFix: (CLLocation) -> Void
    private let onAuthorized: () -> Void

    init(onFix: @escaping (CLLocation) -> Void,
         onAuthorized: @escaping () -> Void) {
        self.onFix = onFix
        self.onAuthorized = onAuthorized
    }

    func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        guard let loc = locations.last else { return }
        onFix(loc)
    }

    func locationManager(_ manager: CLLocationManager, didFailWithError error: Error) {
        // Best-effort — GPS isn't required for OCR. (Common indoors: kCLErrorLocationUnknown.)
    }

    func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        switch manager.authorizationStatus {
        case .authorizedWhenInUse, .authorizedAlways:
            onAuthorized()
        default:
            break
        }
    }
}
