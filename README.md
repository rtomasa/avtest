# Convert PNG to raw RGB (no alpha)
convert grid_50.png -resize 320x288! -depth 8 -alpha off -define quantum:format=unsigned-integer -type truecolor -set colorspace RGB rgb:grid_50.bin
convert grid_60.png -resize 320x240! -depth 8 -alpha off -define quantum:format=unsigned-integer -type truecolor -set colorspace RGB rgb:grid_60.bin

# Convert raw data to C header
xxd -i -c 12 grid_50.bin > images.h
xxd -i -c 12 grid_60.bin >> images.h
{ echo "/* Auto-generated from Left.wav and Right.wav. */"; xxd -i Left.wav; xxd -i Right.wav; } > audio_data.c
