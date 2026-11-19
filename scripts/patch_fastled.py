# Pin FastLED 3.7.8 and gate every RMT pin dispatch through the board power interlock.
Import("env")
from pathlib import Path

def patch(node):
    p = Path(node.srcnode().get_abspath())
    s = p.read_text()
    s = s.replace("mMaxCyclesPerFill = mCyclesPerFill + mCyclesPerFill / 2;", "mMaxCyclesPerFill = 2 * mCyclesPerFill;")
    old = "espErr = rmt_set_gpio(mRMT_channel, RMT_MODE_TX, mPin, false);"
    new = "l8_rmt_route((int)mRMT_channel, (int)mPin);"
    if new not in s:
        if old not in s: raise RuntimeError("FastLED RMT interlock patch no longer matches; refusing an ungated build")
        s = s.replace(old,new)
        s = 'extern "C" void l8_rmt_route(int channel, int pin);\n' + s
    p.write_text(s)
    return node

env.AddBuildMiddleware(patch, "*idf4_rmt_impl.cpp")
