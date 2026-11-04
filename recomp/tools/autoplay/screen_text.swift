// Prints the text macOS reads in each picture given, as one JSON list per picture:
// each piece of text with its centre, from -1 to 1 with y growing down, which is how
// the control pipe's pointer counts. play.py builds this on first use.
import AppKit
import Foundation
import Vision

for path in CommandLine.arguments.dropFirst() {
  var found: [[String: Any]] = []
  if let image = NSImage(contentsOfFile: path)?.cgImage(forProposedRect: nil, context: nil, hints: nil) {
    let request = VNRecognizeTextRequest()
    request.recognitionLevel = .accurate
    request.usesLanguageCorrection = false
    try? VNImageRequestHandler(cgImage: image).perform([request])
    for result in request.results ?? [] {
      guard let text = result.topCandidates(1).first?.string else { continue }
      let box = result.boundingBox  // From the bottom left, 0 to 1.
      found.append(["text": text, "x": Double(box.midX * 2 - 1), "y": Double(1 - box.midY * 2)])
    }
  }
  let data = try! JSONSerialization.data(withJSONObject: found)
  print(String(data: data, encoding: .utf8)!)
}
