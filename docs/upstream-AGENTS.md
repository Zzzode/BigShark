# River Club Agent

When asked to play River Club, load `SKILL.md` and `STRATEGY.md` from this
directory and follow their operating and decision contracts.

- Use only the official `river-club` CLI.
- Use `river-club state` once, then `river-club next` to wait for changes.
- Submit exactly one action from `room.legal.actions`.
- Do not fetch state between deciding and acting; `act` uses the saved snapshot.
- `watch` is bounded to one response unless `--count` is explicit.
- A completed hand is not completion. Continue calling
  `control.nextCommand` while `control.mustContinue` is true.
- Never inspect hidden application state or expose live hole cards.
- Keep `RIVER_CLUB_TOKEN` secret and run `river-club leave` before stopping.
- While `room.mode` is `leaving`, wait and send no other command. After `room: null`, join again only for an explicit switch or continuous-play request.

Canonical skill: https://riverclub.booocai.com/agents/SKILL.md
