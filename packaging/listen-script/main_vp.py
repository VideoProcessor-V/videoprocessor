# main.py (the current dimming revision; its rules are identical to beta_22.py)
# with the VideoProcessor listening addendum (2026-10-13).
#
# VideoProcessor's copy (scripts\main_vp.py): set
#   [led_drive.Default]  command_source: python  and  listen_script: scripts\main_vp.py
# and VP starts, watches and restarts it. Installed only when absent: an
# edited copy is never replaced by an update.
#
# The only changes from main.py are marked "VP addendum". After every
# command the script sends to COM1 it also tells VideoProcessor, on this PC
# only (UDP to 127.0.0.1:47811), what it sent, and once per loop what it
# measured. With command_source: python, VideoProcessor then:
#   - relays each command to the projector exactly as this script wrote it
#     (VP holds COM1 and the script echoes to nul; see _vp_com1 below), or,
#     with python_drives_port, leaves COM1 to this script as before,
#   - with adaptive_gamma on, eases the picture across each contrast step
#     (the script-mode smoothing), and
#   - runs its own copy of this script's rules on its own measurement and
#     logs how closely they agree (the accuracy of VP's approximation), and
#   - with python_feed (the default), answers this script's request for the
#     mean and max of the frame on screen (its own measurement), so the script skips its
#     screenshot (see vp_feed below; the loop keeps its pace).
# Started by hand, or with VideoProcessor closed, the messages go nowhere and
# the script writes COM1 itself: it works exactly as before.
import time
import numpy as np
import pyautogui
import cv2
import subprocess
import os

# --- VP addendum: tell VideoProcessor what was sent and measured ---------
import socket
_vp_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
_vp_state = {"mean": -1, "max": -1, "hl": -1, "tp": -1, "tc": -1}

def vp_notify(kind, value=-1):
    # One line per message; never blocks and never raises (VP may be closed).
    try:
        message = "VP1 %s %d t=%.3f mean=%d max=%d hl=%d tp=%d tc=%d" % (
            kind, int(value), time.time(), _vp_state["mean"], _vp_state["max"],
            _vp_state["hl"], _vp_state["tp"], _vp_state["tc"])
        _vp_socket.sendto(message.encode("ascii"), ("127.0.0.1", 47811))
    except Exception:
        pass
# Relay (VP 2026-10-14): when VideoProcessor starts this script with
# VP_LED_RELAY=1, VP holds COM1 and sends each command itself, exactly as
# this script would have written it. The script then echoes to nul instead
# of com1: the same cmd call, so its timing is unchanged.
_vp_com1 = "nul" if os.environ.get("VP_LED_RELAY") == "1" else "com1"
# Feed (VP 2026-10-15): when VideoProcessor starts this script with
# VP_LED_FEED=1, each loop asks VP for the picture's mean and max instead of
# taking a screenshot - VP's measurement of the frame on screen, the same
# frame the screenshot would have seen. The loop then waits as long as the screenshot took (measured on real
# screenshots), so the script's pace and ramps are unchanged. No answer from
# VP: that loop takes its screenshot as before.
_vp_feed = os.environ.get("VP_LED_FEED") == "1"
_vp_shot_seconds = None   # the screenshot + resize time, smoothed
_vp_request = 0

def vp_feed():
    # (mean2, max_val, highlightdetect) from VP, or None. Waits at most 0.15 s.
    global _vp_request
    if not _vp_feed:
        return None
    _vp_request += 1
    try:
        vp_notify('REQ', _vp_request)
        deadline = time.time() + 0.15
        while True:
            left = deadline - time.time()
            if left <= 0:
                return None
            _vp_socket.settimeout(left)
            data, _ = _vp_socket.recvfrom(512)
            words = data.decode("ascii", "replace").split()
            if not words or words[0] != "VPS1":
                continue
            fields = dict(w.split("=", 1) for w in words[1:] if "=" in w)
            if fields.get("id") != str(_vp_request):
                continue   # a late answer to an earlier request
            if "none" in words[1:]:
                return None
            return int(fields["mean"]), int(fields["max"]), int(fields["hl"])
    except Exception:
        return None
    finally:
        try:
            _vp_socket.settimeout(None)
        except Exception:
            pass

def vp_shot_time(seconds):
    # The screenshot path's duration, for the feed's pause.
    global _vp_shot_seconds
    _vp_shot_seconds = seconds if _vp_shot_seconds is None else _vp_shot_seconds + (seconds - _vp_shot_seconds) * 0.2
# --- end VP addendum ------------------------------------------------------
#subprocess.call(('echo :lpw1{}cr' .format(mean2)), shell=True)
# VP copy: the Tera Term start that set COM1's line settings is removed;
# VideoProcessor sets the port ([projector.Default] port / baud, 8N1) before
# it starts this script (in relay VP holds the port itself instead).
#LED Power
subprocess.call('echo :lpw1255cr > ' + _vp_com1, shell=True)   # VP addendum: _vp_com1
vp_notify('LPW1', 255)   # VP addendum
current_contrast = 255
#Contrast
subprocess.call('echo :cntr50cr > ' + _vp_com1, shell=True)   # VP addendum: _vp_com1
vp_notify('CNTR', 50)   # VP addendum
current_con = 50

#sudden flag
sudden_flag = 0

_vp_shots = 0   # VP addendum: screenshots timed so far

while(True):
    # VP addendum: VP's measurement instead of the screenshot (VP_LED_FEED=1),
    # once three real screenshots have been timed for the pause.
    _vp_start = time.time()
    _vp_fed = vp_feed() if _vp_shots >= 3 else None
    if _vp_fed is not None:   # VP addendum
        mean2, max_val, highlightdetect = _vp_fed
        mean = mean2
        print(max_val)
        print(mean2)
        # The screenshot's time, so the loop keeps its pace.
        _vp_left = _vp_shot_seconds - (time.time() - _vp_start)
        if _vp_left > 0:
            time.sleep(_vp_left)
    else:   # VP addendum: the original screenshot measurement, indented
        _vp_start = time.time()   # VP addendum: time the screenshot alone
        #take screenshot
        image = pyautogui.screenshot()
        #convert to grayscale
        image = cv2.cvtColor(np.array(image), cv2.COLOR_BGR2GRAY)
        #resize image
        width = 854
        height = 480
        dim = (width, height)
        resized = cv2.resize(image, dim, interpolation=cv2.INTER_AREA)
        # get image max value
        min_val, max_val, min_indx, max_indx = cv2.minMaxLoc(resized)
        print(max_val)
        if max_val >= 215:
            highlightdetect = 1
        if max_val <215:
            highlightdetect = 0
        #crop image
        y=57
        x=0
        h=366
        w=854
        cropped = resized[y:y+h, x:x+w]
        #take image mean
        mean = np.mean(cropped)
        print(mean)
        mean2 = int(mean)
        print(mean2)
        vp_shot_time(time.time() - _vp_start)   # VP addendum
        _vp_shots += 1                          # VP addendum

    #Set power target based upon mean2:
    if mean2 >= 125:
        target_power = 255
        print('target_power 255')
        target_contrast = 50
        #contrast_flag = 0
    if 100 <= mean2 < 125:
        target_power = 255
        print('target_power 255')
        target_contrast = 50
        #contrast_flag = 0
    if 70 <= mean2 < 100:
        calculate_pwr = (((mean2 - 70) * 0.8333) + 230)
        calculate_pwr_final = int(calculate_pwr)
        target_power = calculate_pwr_final
        print('target_power {}'.format(target_power))
    if 40 <= mean2 < 70:
        calculate_pwr = (((mean2 - 40) * 1) + 200)
        calculate_pwr_final = int(calculate_pwr)
        target_power = calculate_pwr_final
        print('target_power {}'.format(target_power))
    if 20 <= mean2 < 40:
        calculate_pwr = (((mean2 - 20) * 5) + 100)
        calculate_pwr_final = int(calculate_pwr)
        target_power = calculate_pwr_final
        print('target_power {}' .format(target_power))
    if 0.0 <= mean2 < 20:
        target_power = 100
        print('target power 100')

    #set contrast target based upon mean2:
    if 60 <= mean2 <100:
        target_contrast = 50
        #contrast_flag = 0
    if 45 <= mean2 < 60:
        #205 --> 220; 166.54 = 0.89 --> ~52 recommended
        if highlightdetect == 1:
            target_contrast = 50
            #calculate_con = ((abs(mean2 - 60) * 0.133) + 50)
            #calculate_con_final = int(calculate_con)
            #target_contrast = calculate_con_final
            #target_contrast = 52
        if highlightdetect == 0:
            target_contrast = 51
            #calculate_con = ((abs(mean2 - 60) * 0.133) + 50)
            #calculate_con_final = int(calculate_con)
            #target_contrast = calculate_con_final
            #target_contrast = 52
        #contrast_flag = 1
    if 30 <= mean2 < 45:
        #150 --> 205; 150 --> 62 recommended
        #if highlightdetect == 1:
        #    calculate_con = ((abs(mean2 - 45) * 0.6667) + 50)
        #    calculate_con_final = int(calculate_con)
        #    target_contrast = calculate_con_final
            #target_contrast = 58
        if highlightdetect != 2:
            calculate_con = ((abs(mean2 - 45) * 0.7333) + 51)
            calculate_con_final = int(calculate_con)
            target_contrast = calculate_con_final
            #target_contrast = 62
        #contrast_flag = 1
    if 20 <= mean2 < 30:
        #100 --> 150; 100 = 74 reccommended
        #if highlightdetect == 1:
        #    calculate_con = ((abs(mean2 - 30) * 0.8) + 60)
        #    calculate_con_final = int(calculate_con)
        #    target_contrast = calculate_con_final
            #target_contrast = 68
        if highlightdetect != 2:
            calculate_con = ((abs(mean2 - 30) * 1.2) + 62)
            calculate_con_final = int(calculate_con)
            target_contrast = calculate_con_final
            #target_contrast = 75
    if 0.0 <= mean2 < 20:
        #60
        #if highlightdetect == 1:
        #    target_contrast = 68
        if highlightdetect != 2:
            target_contrast = 74
        #contrast_flag = 1

    #Highlight detection anticrush:
    #if max_val > 240:
        #target_contrast = 50

    # VP addendum: what this loop measured and decided
    _vp_state.update(mean=mean2, max=int(max_val), hl=highlightdetect, tp=target_power, tc=target_contrast)
    vp_notify('TICK')
    #Power change rate determination and setting:
    if current_contrast != target_power:
        difference = abs(target_power - current_contrast)
        if difference >= 100:
            if target_power > current_contrast:
                set_power = (current_contrast + 7)
                #set_power = (target_power)
                current_contrast = set_power
                #sudden_flag = 1
                #if current_con != target_contrast:
                #    set_contrast = (target_contrast)
                #    current_con = set_contrast
                #    subprocess.call(('echo :cntr{}cr > com1'.format(set_contrast)), shell=True)
                #    time.sleep(200 / 1000)
            if target_power < current_contrast:
                set_power = (current_contrast - 5)
                current_contrast = set_power
        if 50 <= difference < 100:
            if target_power > current_contrast:
                if mean2 < 40:
                    set_power = (current_contrast + 5)
                    current_contrast = set_power
                if mean2 >= 40:
                    set_power = (current_contrast + 4)
                    current_contrast = set_power
            if target_power < current_contrast:
                set_power = (current_contrast - 3)
                current_contrast = set_power
        if 25 <= difference < 50:
            if target_power > current_contrast:
                if mean2 < 40:
                    set_power = (current_contrast + 4)
                    current_contrast = set_power
                if mean2 >= 40:
                    set_power = (current_contrast + 2)
                    current_contrast = set_power
            if target_power < current_contrast:
                set_power = (current_contrast - 2)
                current_contrast = set_power
        if 10 <= difference < 25:
            if target_power > current_contrast:
                set_power = (current_contrast + 1)
                current_contrast = set_power
            if target_power < current_contrast:
                set_power = (current_contrast - 1)
                current_contrast = set_power
        if difference < 10:
            if target_power > current_contrast:
                set_power = (current_contrast + 1)
                current_contrast = set_power
            if target_power < current_contrast:
                set_power = (current_contrast - 1)
                current_contrast = set_power
        subprocess.call(('echo :lpw1{}cr > {}'.format(set_power, _vp_com1)), shell=True)   # VP addendum: _vp_com1
        vp_notify('LPW1', set_power)   # VP addendum
        subprocess.call(('echo :lpw1{}cr'.format(set_power)), shell=True)
        time.sleep(200/1000)

    #Contrast change rate determination and setting:
    if current_con != target_contrast:
        difference2 = abs(target_contrast - current_con)
        if sudden_flag == 1:
            set_contrast = (target_contrast)
            current_con = set_contrast
        if sudden_flag == 0:
            if 10 <= difference2:
                if target_contrast > current_con:
                    set_contrast = (current_con + 1)
                    current_con = set_contrast
                if target_contrast < current_con:
                    if highlightdetect == 1:
                        while 10 < difference2:
                            set_contrast = (current_con - 1)
                            current_con = set_contrast
                            time.sleep(100 / 1000)
                            subprocess.call(('echo :cntr{}cr > {}'.format(set_contrast, _vp_com1)), shell=True)   # VP addendum: _vp_com1
                            vp_notify('CNTR', set_contrast)   # VP addendum
                            difference2 = abs(target_contrast - current_con)
                        if difference2 == 10:
                            set_contrast = (current_con - 1)
                            current_con = set_contrast
                        #set_contrast = (current_con - 2)
                    if highlightdetect == 0:
                        set_contrast = (current_con - 1)
                    current_con = set_contrast
            if 7 <= difference2 < 10:
                if target_contrast > current_con:
                    set_contrast = (current_con + 1)
                    current_con = set_contrast
                if target_contrast < current_con:
                    while 7 <= difference2 < 10:
                        set_contrast = (current_con - 1)
                        current_con = set_contrast
                        time.sleep(100 / 1000)
                        subprocess.call(('echo :cntr{}cr > {}'.format(set_contrast, _vp_com1)), shell=True)   # VP addendum: _vp_com1
                        vp_notify('CNTR', set_contrast)   # VP addendum
                        difference2 = abs(target_contrast - current_con)
                    #if highlightdetect == 1:
                        #set_contrast = (current_con - 1)
                    #if highlightdetect ==0:
                        #set_contrast = (current_con - 1)
                    #current_con = set_contrast
            if 1 <= difference2 < 7:

                #antipump code for camera changes in low light scenes - only in scenes with mean2<60
                    #if highlight detected allow contrast changes as per normal in order to prevent crushing

                if 6 <= difference2 < 7:
                    if target_contrast > current_con:
                        if highlightdetect == 1:
                            set_contrast = (current_con + 1)
                        if highlightdetect == 0:
                            set_contrast = (current_con + 1)
                        current_con = set_contrast
                        contrast_incr = 2
                    if target_contrast < current_con:
                        if highlightdetect == 1:
                            set_contrast = (current_con - 1)
                        if highlightdetect == 0:
                            set_contrast = (current_con - 1)
                        current_con = set_contrast
                if 4 <= difference2 < 6:
                    if target_contrast > current_con:
                        if highlightdetect == 1:
                            set_contrast = (current_con + 1)
                        if highlightdetect == 0:
                            set_contrast = (current_con + 1)
                        current_con = set_contrast
                        contrast_incr = 1
                    if target_contrast < current_con:
                        if highlightdetect == 1:
                            set_contrast = (current_con - 1)
                        if highlightdetect == 0:
                            set_contrast = (current_con - 1)
                        current_con = set_contrast
                if 1 <= difference2 < 4:
                    if target_contrast > current_con:
                        if highlightdetect == 1:
                            set_contrast = (current_con + 0)
                        if highlightdetect == 0:
                            set_contrast = (current_con + 1)
                        current_con = set_contrast
                        contrast_incr = 1
                    if target_contrast < current_con:
                        if highlightdetect == 1:
                            set_contrast = (current_con - 1)
                        if highlightdetect == 0:
                            set_contrast = (current_con - 0)
                        current_con = set_contrast
                #if mean2 >= 75:
                    #no antipump set if scene is getting brighter than contrast increase threshold
                    #if 4 <= difference2 < 7:
                        #if target_contrast > current_con:
                            #set_contrast = (current_con + 1)
                            #current_con = set_contrast
                            #contrast_incr = 2
                        #if target_contrast < current_con:
                            #set_contrast = (current_con - 1)
                            #current_con = set_contrast
                    #if 1 <= difference2 < 4:
                        #if target_contrast > current_con:
                            #set_contrast = (current_con + 1)
                            #current_con = set_contrast
                            #contrast_incr = 1
                        #if target_contrast < current_con:
                            #set_contrast = (current_con - 1)
                            #current_con = set_contrast
#        if max_val >200 and current_con >= 55:
#            set_contrast = (current_con - contrast_incr - 5)
#            current_con = set_contrast
#        if max_val >200 and 55 < current_con > 50:
#            current_con = set_contrast
        time.sleep(200/1000)
        subprocess.call(('echo :cntr{}cr > {}'.format(set_contrast, _vp_com1)), shell=True)   # VP addendum: _vp_com1
        vp_notify('CNTR', set_contrast)   # VP addendum


     #       if 0 <= mean2 < 0.5:
     #           subprocess.call('echo echo:lpw140cr', shell=True)
    #            subprocess.call('echo :lpw140cr > com1', shell=True)
#        print(set_power)
#        print('power change')
#            current_contrast = set_power
#    current_contrast = set_power
#    time.sleep(300/1000)
