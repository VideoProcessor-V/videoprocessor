# main.py (the current dimming revision; its rules are identical to beta_22.py)
# with the VideoProcessor listening addendum (2026-10-13).
#
# VideoProcessor's copy (scripts\main_vp.py): set
#   [led_drive.Default]  mode: listen  and  listen_script: scripts\main_vp.py
# and VP starts, watches and restarts it. Installed only when absent: an
# edited copy is never replaced by an update.
#
# The only changes from main.py are marked "VP addendum". After every
# command the script sends to COM1 it also tells VideoProcessor, on this PC
# only (UDP to 127.0.0.1:47811), what it sent, and once per loop what it
# measured. VideoProcessor's LED mode "listen" then:
#   - eases the picture across each contrast step exactly as if VP had sent
#     the command itself (the script-mode smoothing), and
#   - runs its own copy of this script's rules on its own measurement and
#     logs how closely they agree (the accuracy of VP's approximation).
# If VideoProcessor is not running, or not in listen mode, the messages go
# nowhere: the script works exactly as before. VP never opens COM1 in listen
# mode, so the port stays this script's.
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
# --- end VP addendum ------------------------------------------------------
#subprocess.call(('echo :lpw1{}cr' .format(mean2)), shell=True)
# VP copy: the Tera Term start that set COM1's line settings is removed;
# VideoProcessor sets the port ([projector.Default] port / baud, 8N1) before
# it starts this script, and never holds the port itself.
#LED Power
subprocess.call('echo :lpw1255cr > com1', shell=True)
vp_notify('LPW1', 255)   # VP addendum
current_contrast = 255
#Contrast
subprocess.call('echo :cntr50cr > com1', shell=True)
vp_notify('CNTR', 50)   # VP addendum
current_con = 50

#sudden flag
sudden_flag = 0

while(True):
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
        subprocess.call(('echo :lpw1{}cr > com1'.format(set_power)), shell=True)
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
                            subprocess.call(('echo :cntr{}cr > com1'.format(set_contrast)), shell=True)
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
                        subprocess.call(('echo :cntr{}cr > com1'.format(set_contrast)), shell=True)
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
        subprocess.call(('echo :cntr{}cr > com1'.format(set_contrast)), shell=True)
        vp_notify('CNTR', set_contrast)   # VP addendum


     #       if 0 <= mean2 < 0.5:
     #           subprocess.call('echo echo:lpw140cr', shell=True)
    #            subprocess.call('echo :lpw140cr > com1', shell=True)
#        print(set_power)
#        print('power change')
#            current_contrast = set_power
#    current_contrast = set_power
#    time.sleep(300/1000)
