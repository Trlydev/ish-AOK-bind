# lib.sh -- what abn.sh and abf.sh share. Sourced; expects SP, udid, host, log.
#
# The power check is here because of 2026-10-12: a day of A10X timings ran its
# battery down to where iPadOS turned Low Power Mode on by itself, which caps
# the CPU, and nobody was told. A device is not timed below 40% (it may be
# charging and still losing ground, as that one was), nor in Low Power Mode;
# the device's own half of the check is in bench.sh and benchf.sh.

s() { ssh -o BatchMode=yes -o ConnectTimeout=8 -o ServerAliveInterval=30 -o ServerAliveCountMax=4 $host "$@"; }

# One install at a time across runs that share this directory. The lock names
# its holder, so a run that was killed does not hold up the next one for ever
# (it did, for ten minutes, the same day).
install_lock() {
    until mkdir "$SP/install.lock" 2>/dev/null; do
        holder=$(cat "$SP/install.lock/pid" 2>/dev/null)
        if [ -n "$holder" ] && ! kill -0 "$holder" 2>/dev/null; then
            rm -f "$SP/install.lock/pid"; rmdir "$SP/install.lock" 2>/dev/null
            continue
        fi
        sleep 5
    done
    echo $$ > "$SP/install.lock/pid"
}
install_unlock() { rm -f "$SP/install.lock/pid"; rmdir "$SP/install.lock" 2>/dev/null; }

# The battery, asked of the device itself (libimobiledevice), before anything
# is installed. No answer is not a refusal: the tool may not be there.
battery_ok() {
    level=$(ideviceinfo -u "$udid" -q com.apple.mobile.battery -k BatteryCurrentCapacity 2>/dev/null)
    case "$level" in ''|*[!0-9]*) return 0 ;; esac
    if [ "$level" -lt 40 ]; then
        echo "REFUSED: battery at $level% on $udid (below 40%): charge it first" | tee -a "$log" >&2
        return 1
    fi
}

install_and_launch() { # <build>
    install_lock
    xcrun devicectl device install app --device $udid "$SP/app-$1/iSH-AOK.app" >> "$log" 2>&1
    install_unlock
    xcrun devicectl device process launch --device $udid --terminate-existing --environment-variables '{"ISH_SESSION_RESUME":"fresh"}' app.ish.iSH-AOK >> "$log" 2>&1
    n=0; until s true 2>/dev/null || [ $n -ge 60 ]; do sleep 10; n=$((n+1)); done
    if ! s true 2>/dev/null; then
        echo "REFUSED: the guest on $udid did not come up after launching $1 (locked? wedged?): stopping" | tee -a "$log" >&2
        return 1
    fi
    sleep 20
    s 'mkdir -p ~/bench558; cat > ~/bench558/bench.sh' < "$SP/bench.sh"
    s 'cat > ~/bench558/benchf.sh' < "$SP/benchf.sh"
}

# The device script said no (Low Power Mode, battery): its line is in the log.
refused() { tail -3 "$log" | grep -q '^REFUSED'; }
