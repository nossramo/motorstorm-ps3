// Local diagnostic image OCR; reads a saved frame, not the desktop.
import Foundation
import Vision
import ImageIO

guard CommandLine.arguments.count == 2,
      let source = CGImageSourceCreateWithURL(URL(fileURLWithPath: CommandLine.arguments[1]) as CFURL, nil),
      let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else { exit(2) }
let request = VNRecognizeTextRequest()
request.recognitionLevel = .accurate
request.usesLanguageCorrection = false
do {
    try VNImageRequestHandler(cgImage: image).perform([request])
} catch {
    fputs("Frame OCR failed: \(error)\n", stderr)
    exit(1)
}
for result in request.results ?? [] {
    if let text = result.topCandidates(1).first { print(text.string) }
}
