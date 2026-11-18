; ============================================================================
;  demo/sma_demo.ks — SMA (skeletal mesh animation) showcase.
;  Run via demo/sma_entry.lua. The actor is spawned by the entry (solid
;  texture injected at runtime; the repo ships no binary PNG assets);
;  this script drives the animation sequence with KAG commands.
; ============================================================================
[font face="default" size=22]
[pt speed=60]

[ch name="System" text="SMA demo: skeletal mesh animation showcase."]
[p]
[ch name="System" text="Bone hierarchy: root -> body -> head + two 2-bone arms; eye part with variants; idle breathing and wave clips; 2-bone IK."]
[p]

; --- idle (looping) ---
[sma_anim name="hero" anim="idle"]
[ch name="System" text="idle: breathing loop (body/head tracks)."]
[wait time=2500]

; --- crossfade to wave ---
[sma_anim name="hero" anim="wave" blend_time=0.4 loop=true]
[ch name="System" text="wave: crossfade for 0.4 seconds, then right-arm oscillation."]
[wait time=3500]

; --- part variant: happy eyes ---
[sma_variant name="hero" part="eyes" variant="happy"]
[ch name="System" text="eyes variant -> happy (part swap)."]
[wait time=2000]

; --- 2-bone IK: right arm reaches a target ---
[sma_ik name="hero" bone0=5 bone1=6 tx=0.95 ty=0.25 l2=0.3]
[ch name="System" text="IK: right arm (2-bone chain) reaches the target point."]
[wait time=2500]

; --- stop ---
[sma_stop name="hero"]
[ch name="System" text="SMA demo complete. Goodbye!"]
[wait time=1000]

[end]
