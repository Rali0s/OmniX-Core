export const siteConfig = {
  brandName: "The Black Lantern Rotation",
  brandMark: "The Black Lantern",
  brandSubmark: "Rotation",
  tagline: "Signal through the fog",
  audience: "Night-shift builders, readers, and the technically curious",
  navItems: [
    { label: "Home", href: "/" },
    { label: "Dispatches", href: "/blog" },
    { label: "Broadcasts", href: "/broadcasts" },
    { label: "Rotation", href: "/rotation" },
  ],
  footerNote:
    "A moody publishing room for dispatches, broadcasts, and the kind of technical curiosity that tends to wake up after midnight.",
  quickHits: [
    {
      label: "Format",
      value: "Essays + audio",
      copy: "The Rotation is built for long-form dispatches, spoken transmissions, and recurring notes that reward patient readers.",
    },
    {
      label: "Tone",
      value: "Low light / high signal",
      copy: "Dark, atmospheric, and deliberate. The mood comes first visually, but the writing still has to earn its keep.",
    },
    {
      label: "Readers",
      value: "Builders + lurkers",
      copy: "For engineers, writers, researchers, and curious wanderers who like their ideas with a little smoke around the edges.",
    },
  ],
  explainer: [
    {
      title: "A lantern for stray signal",
      description:
        "The Black Lantern Rotation is a home for technical essays, cultural field notes, and midnight-grade observations that feel too sharp to leave in drafts.",
    },
    {
      title: "A recurring room, not a feed dump",
      description:
        "The Rotation works like an orbit: dispatches return to the same obsessions, themes, and questions, each pass catching a different angle of the light.",
    },
    {
      title: "Atmosphere with actual substance",
      description:
        "The image, the fog, the glow, and the dark palette are part of the identity, but they are there to carry the writing, not distract from it.",
    },
  ],
  audienceProfiles: [
    {
      title: "People who read code and prose",
      description:
        "You like systems thinking, networked culture, weird workflows, and essays that trust you to follow a hard thought all the way through.",
    },
    {
      title: "People learning by mood and detail",
      description:
        "You do not need to arrive as an expert. The Rotation is for readers who follow the light, then stay because the writing takes them somewhere real.",
    },
  ],
  operatingStyle: [
    {
      title: "Keep the signal visible",
      description:
        "Mystery is welcome. Confusion is not. The atmosphere should deepen the message, not bury it.",
    },
    {
      title: "Write like a field note",
      description:
        "Each piece should feel observed, gathered, and carried back from somewhere worth visiting.",
    },
    {
      title: "Let the glow do the framing",
      description:
        "The site can look haunted and elegant as long as the reader never loses the thread.",
    },
  ],
  podcastReadiness: [
    {
      title: "Broadcast route ready",
      description:
        "The audio page is already shaped like a broadcast room, so future feeds and episode links can drop in without changing the architecture.",
    },
    {
      title: "Internal now, external later",
      description:
        "Until there is a real feed, the broadcast page works as a branded holding space rather than a dead-end placeholder.",
    },
    {
      title: "Same world, different medium",
      description:
        "The voice does not change when the text becomes audio. It still sounds like signal carried through a little weather.",
    },
  ],
} as const

export const valueProps = [
  {
    eyebrow: "The Watch",
    title: "A place to keep noticing things",
    description:
      "The site is built for recurring returns: ideas, systems, fragments, patterns, and whatever keeps glowing after the first read.",
  },
  {
    eyebrow: "The Rotation",
    title: "Dispatches in cycles, not noise",
    description:
      "Instead of chasing every passing trend, the Rotation circles a set of obsessions and lets them deepen over time.",
  },
  {
    eyebrow: "The Glow",
    title: "Style with a pulse underneath",
    description:
      "The black-and-gold mood is intentional, but the point is still the writing, the broadcast, and the signal worth preserving.",
  },
] as const

export const podcastHighlights = [
  {
    label: "Field transmission",
    title:
      "Long-form broadcasts for ideas that need a little darkness around them.",
    description:
      "Audio space for essays read aloud, technical meditations, and late-night thought lines that sound better with breath in them.",
  },
  {
    label: "Night watch",
    title: "Shorter recordings when one sharp observation is enough.",
    description:
      "Small transmissions, passing warnings, luminous fragments, and pieces that arrive before they are overexplained.",
  },
  {
    label: "Signal sweep",
    title: "A recurring scan of what is worth carrying forward.",
    description:
      "Patterns, themes, books, systems, links, or ideas that feel like they belong in the same orbit even if they come from different rooms.",
  },
] as const

export const rotationPrinciples = [
  {
    eyebrow: "Rule 01",
    title: "Atmosphere serves meaning",
    description:
      "The fog, glow, and shadow are part of the invitation, but the writing still needs to land with clarity.",
  },
  {
    eyebrow: "Rule 02",
    title: "Mystery gets structure",
    description:
      "Even the most haunted-feeling page should remain calm to navigate and easy to read.",
  },
  {
    eyebrow: "Rule 03",
    title: "Every dispatch leaves a trail",
    description:
      "A piece should point toward the next question, the next room, or the next rotation rather than ending in empty style.",
  },
] as const
