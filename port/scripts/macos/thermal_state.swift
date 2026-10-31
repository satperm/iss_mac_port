// Prints macOS's thermal state: 0 nominal, 1 fair, 2 serious, 3 critical (bench.sh cools down on it).
import Foundation
print(ProcessInfo.processInfo.thermalState.rawValue)
