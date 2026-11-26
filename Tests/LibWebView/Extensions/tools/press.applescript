on run argv
  set target to item 1 of argv
  tell application "System Events" to tell process "Ladybird"
    set frontmost to true
    set els to entire contents of window 1
    repeat with e in els
      try
        if (description of e as text) is target or (name of e as text) is target or (title of e as text) is target then
          perform action "AXPress" of e
          return "pressed " & (role of e as text)
        end if
      end try
    end repeat
  end tell
  return "not found"
end run
