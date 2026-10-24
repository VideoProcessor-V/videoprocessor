VIDEOPROCESSOR SETTINGS PREVIEW
===============================

What this is
------------
A copy of the VideoProcessor Configuration app that you can click through to
review how the settings are organised and to see every option. It runs on its
own: VP does not need to be running, and no capture card or projector is needed.


How to open it
--------------
Double-click  "Open settings preview.bat"  in this folder.

The window title ends in "Settings preview (sandbox file)". Close it with the
X as usual. You can open it as often as you like.

Where to look first:
  - The "VP-V" page (left-hand list, near the bottom). It holds everything VP
    adds, with one tab per unit: Input, Motion, Repair, Scaling, Colour,
    Dynamic Drive, Output, then "Pipeline (expert)" and "Proposed retirement".
  - Each unit tab shows its tier slider (Classic / Low / Mid / Upper) where the
    unit has steps, the settings that belong to it, and a line saying where the
    rest of that unit's settings are on the other pages.
  - The other pages (General, Queue, Rendering, Motion, Scaling, Color / Output,
    Dynamic Drive, Shortcuts, ...) are the same pages as the real app.


Nothing real is touched
-----------------------
  - Every change you make is saved ONLY into  sandbox\VideoProcessor.cfg  in
    this folder. Your real settings in C:\Videoprocessor are never opened or
    changed.
  - The preview never talks to a running VP, the projector or other computers.
    Buttons that need VP (run a profile or action, projector connection test,
    calibration, updates) just say that VP is not connected.
  - To start again from the original sandbox, copy
    sandbox\VideoProcessor.original.cfg over sandbox\VideoProcessor.cfg.

The sandbox starts as the cinema set-up (the madVR-like configuration for the
FL35) with VP's current built-in defaults for everything it does not set.
"Not set (default: ...)" or "Default: ..." in a box means VP's built-in value.


What is real and what is a placeholder
--------------------------------------
Real (does what it says once it is in the real configuration):
  - Every box, switch and slider that you can change. Each one edits a real VP
    setting, and Apply / OK saves it the same way the real app does.
  - The tier sliders: a tier only sets the listed settings to fixed values. If
    you change one of those settings by hand the tier shows "Custom".
  - "Pipeline (expert)": the stage order and stage on/off for testing.

Placeholder (shown so you can see the whole plan):
  - Greyed check boxes marked "not yet implemented": parts of the design that
    are not built yet (for example the encoding-repair net, the 4K refine net,
    black level correction).
  - Tier steps marked "not yet implemented" (for example Scaling: Upper).
  - Status lines such as "Fallbacks engaged: (no status from a running VP)" and
    the live profile indicators: they only fill in when connected to VP.

"Proposed retirement" tab
-------------------------
This tab lists the settings proposed for retirement (from the settings guide,
section 12), in four groups: fixes that are proven, experiments that lost,
settings superseded by the new design, and diagnostics to hide. Nothing has
been removed. The same note also appears in each of those settings' tooltips
on the other pages. Tell me yes or no for each group (or for single settings).


How to send feedback
--------------------
  1. Take a screenshot of what you are commenting on: press Windows + Shift + S
     and drag around the area (it goes to the clipboard; paste it into the
     chat), or use the Snipping Tool to save a file.
  2. Add a short note with the page and tab name, for example
     "VP-V > Scaling: this label is unclear" or "Rendering page: why is this
     here?".
  3. If you changed settings and want me to see exactly what you chose, also
     send the file  sandbox\VideoProcessor.cfg .

Useful things to tell me: labels you do not understand, settings that seem to
be in the wrong place, things you expected but could not find, and settings you
would remove.
