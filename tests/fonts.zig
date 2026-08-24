//! The committed test fonts, embedded so the suite needs no files at runtime
//! and no path on the command line.
//!
//! All four are Noto, licensed under the SIL Open Font License 1.1. Their
//! provenance, the exact source URL and a SHA-256 for each are recorded in
//! `fonts/PROVENANCE.md`, and `ci/verify-vendor.sh` checks those hashes -- the
//! golden shaping results in the suite are only meaningful against these exact
//! bytes.
//!
//! Each is here for a reason:
//!
//!   latin   Noto Sans. Standard ligatures and real kerning pairs, so `liga`
//!           and `kern` can be turned on and off and the difference observed.
//!   arabic  Noto Naskh Arabic. Cursive joining -- the same letter takes an
//!           initial, medial, final or isolated form depending on neighbours,
//!           which is the shaping behaviour no advance table can fake.
//!   hebrew  Noto Sans Hebrew. Right-to-left WITHOUT joining, so a bug in
//!           direction handling cannot hide behind a bug in joining.
//!   variable
//!           Noto Sans Hebrew again, as a variable font with `wdth` and
//!           `wght` axes. Two axes rather than one, so a test can prove that
//!           moving one leaves the other alone -- and the same script as
//!           `hebrew`, so the two can be compared directly. It is covered by
//!           the SAME licence file as `hebrew`, since it is the same family
//!           from the same upstream repository; there is deliberately no
//!           second copy of the OFL for it.

pub const latin = @embedFile("fonts/NotoSans-Regular.ttf");
pub const arabic = @embedFile("fonts/NotoNaskhArabic-Regular.ttf");
pub const hebrew = @embedFile("fonts/NotoSansHebrew-Regular.ttf");
pub const variable = @embedFile("fonts/NotoSansHebrew[wdth,wght].ttf");
