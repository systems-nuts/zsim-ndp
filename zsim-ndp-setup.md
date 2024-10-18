# ZSIM Setup

## File Structure
```
|- NDP-PROJECT
    |- zsim/
    |- zsim-env/
        |- bin
        |- include
        |- lib
        |- share
        |- pin-3.11
        |- zsim3-thirdparty-src
            |- libpin3c_missing
            |- pincrt_build
                |- hdf5
                |- libconfig
                |- ...
```
From now on, we assume we are at the `NDP_PROJECT/` folder, which is referred to by `$\` from now on.
The actual name and path of `NDP_PROJECT/` can be specified arbitrarily.

## Install Dependencies of ZSIM
- `mkdir zsim-env && cd zsim-env`
- Install pin
    - 
- Clone pincrt_build
    - `git clone --recursive git@github.com:tsinghua-ideal/pincrt_build.git`
- Clone libpin3c_missing
    - `git clone git@github.com:tsinghua-ideal/libpin3c_missing.git`
- `


## Clone ZSIM
- `git clone git@github.com:CriusT/zsim.git`
- `git checkout -b ndp-bank origin/ndp-bank`

