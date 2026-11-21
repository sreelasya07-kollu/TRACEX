import XCTest
@testable import CaptureKit

final class CaptureKitTests: XCTestCase {
    func testVersion() {
        XCTAssertEqual(CaptureKit.version, "0.0.1")
    }
}
