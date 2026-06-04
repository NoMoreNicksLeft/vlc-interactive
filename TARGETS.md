# iMKV Format — Known Interactive Video Works

This document catalogs known interactive video works that are candidates
for conversion to or authoring in the iMKV format. It is maintained here
temporarily and will migrate to the `imkv-spec` repository when that is
established.

---

## Netflix Interactive (via Eveep23/Interactive-Player compatibility list)

The following titles were produced by Netflix using their proprietary
interactive format. Conversion to iMKV requires the source video and the
corresponding interactive manifest JSON, which encodes branching logic,
segment boundaries, choice point timing, and state variables.

Black Mirror: Bandersnatch is the primary reference implementation for
the iMKV format and the VLC chapter script engine.

- Black Mirror: Bandersnatch *(primary reference implementation)*
- Minecraft Story Mode (Episodes 1-5)
- Cat Burglar
- Buddy Thunderstruck: The Maybe Pile
- Headspace: Unwind Your Mind
- Escape the Undertaker
- Carmen Sandiego: To Steal or Not to Steal
- Barbie: Epic Road Trip
- Puss in Book: Trapped in an Epic Tale
- Stretch Armstrong: The Breakout
- Johnny Test's Ultimate Meatloaf Quest
- Spirit Riding Free: Ride Along Adventure
- The Last Kids on Earth: Happy Apocalypse to You
- Unbreakable Kimmy Schmidt: Kimmy vs. the Reverend
- Captain Underpants: Epic Choice-O-Rama
- The Boss Baby: Get That Baby
- Trivia Quest (Episodes 1-30)
- Jurassic World Camp Cretaceous: Hidden Adventure
- Animals on the Loose: A You vs Wild Movie
- You vs Wild: Out Cold
- Ranveer vs Wild with Bear Grylls
- You vs Wild (Episodes 1-8)
- We Lost Our Human
- Choose Love
- Battle Kitty (Episodes 1-9)

---

## Theatrical and Physical Media

These works predate streaming interactive formats. Conversion to iMKV
requires physical media acquisition, ripping, and reconstruction of the
branching logic from the original release format.

### I'm Your Man (1992, Interfilm)
- Status: Procurable. DVD release 1999, available on eBay.
- Original format: Theatrical with Interfilm joystick system (audience
  aggregate voting). Home release approximates this via DVD menu navigation.
- Conversion path: Rip DVD, reconstruct chapter structure from menu
  logic, author iMKV chapter scripts.
- Notes: First commercially released interactive theatrical film in
  the United States. Historically significant.

### Mr. Payback: An Interactive Movie (1995, Interfilm)
- Status: Presumed lost. Laserdisc only, production run of approximately
  50 copies. None have surfaced publicly.
- Original format: Theatrical with Interfilm joystick system. No known
  home release with branching intact.
- Conversion path: If a Laserdisc copy surfaces: acquire, rip via
  Domesday Duplicator, decode RF to composite, reconstruct branching from
  disc chapter/angle structure.
- Notes: If preserved, would represent a significant archival achievement.
  The interactive logic would need to be reconstructed from the disc
  structure as no manifest data is known to exist.

### Kinoautomat (1967, dir. Raduz Cincera, Czechoslovakia)
- Status: Preservation project. 35mm only. Requires full remaster
  before iMKV conversion is feasible.
- Original format: Theatrical. An actor paused the film and polled the
  audience verbally; branching was executed by the projectionist switching
  reels. No electronic interactivity.
- Conversion path: Locate extant 35mm print, arrange professional scan
  and remaster, reconstruct branching from documented story structure,
  author iMKV chapter scripts.
- Notes: The first known interactive film in history. Its interaction
  model (live audience vote, human execution) maps cleanly onto iMKV's
  Menu() primitive despite the radically different original delivery
  mechanism. To be cited in the iMKV spec background section as the
  origin of the form.

---

## Other

### YouTube Interactive Videos
- Status: Various. YouTube's native interactive format uses end-screens
  and cards, which are not directly convertible.
- Conversion path: TBD. Would require reconstructing the branching
  logic from the video structure and authoring new iMKV chapter scripts.

### Corporate Training Video Genre
- Status: Broad category. Many titles exist across DVD and streaming
  platforms.
- Conversion path: TBD per title.
- Notes: This genre has used interactive branching since the DVD era
  and represents a significant body of existing interactive content.
  iMKV should be expressive enough to encode typical training video
  branching (scenario-based learning, consequence branches, knowledge
  checks).

---

## Notes on Scope

The iMKV format is intended to be agnostic to the original delivery
mechanism and input method. Whether choices were made by a single viewer
pressing a key, a theater audience voting with joysticks, a show of
hands, or a corporate trainer clicking a mouse -- all produce the same
signal at the format level: an option was selected. The file format does
not encode the input mechanism, only the branching structure and
presentation.

There is no maximum number of options per choice point. Authors are
responsible for ensuring their choice presentations are legible given
their chosen layout and placement parameters.
