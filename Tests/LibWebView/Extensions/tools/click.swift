import CoreGraphics
import Foundation
// click.swift x y [right]
let x = Double(CommandLine.arguments[1])!, y = Double(CommandLine.arguments[2])!
let right = CommandLine.arguments.count > 3 && CommandLine.arguments[3] == "right"
let p = CGPoint(x: x, y: y)
let down: CGEventType = right ? .rightMouseDown : .leftMouseDown
let up: CGEventType = right ? .rightMouseUp : .leftMouseUp
let button: CGMouseButton = right ? .right : .left
CGEvent(mouseEventSource: nil, mouseType: .mouseMoved, mouseCursorPosition: p, mouseButton: button)?.post(tap: .cghidEventTap)
usleep(150000)
CGEvent(mouseEventSource: nil, mouseType: down, mouseCursorPosition: p, mouseButton: button)?.post(tap: .cghidEventTap)
usleep(60000)
CGEvent(mouseEventSource: nil, mouseType: up, mouseCursorPosition: p, mouseButton: button)?.post(tap: .cghidEventTap)
