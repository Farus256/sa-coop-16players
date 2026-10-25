<p align="center">
  <img src="docs/images/logo.png" alt="SA:COOP" width="440">
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue" alt="MIT license"></a>
  <img src="https://img.shields.io/badge/GTA_San_Andreas-1.0_US-orange" alt="GTA San Andreas 1.0 US">
  <img src="https://img.shields.io/badge/players-2–16-green" alt="2–16 players">
</p>

<p align="center"><a href="README.md">Русский</a> · <b>English</b></p>

SA:COOP is a mod for GTA San Andreas that lets two to sixteen players go through the story together over the
network. One player is the host: the game runs on their save, and they start the missions. The others connect to
them and play in the same world as characters of their own, with their own money, skills and look.

## Differences from the original

It is based on the original 0.2.7 and keeps everything from it. Added:

- up to 16 players; the host sets the number of places in the F9 window or in `coop\server.ini` (`max_players`,
  4 by default)
- other players' cars and pedestrians only appear near you, and distant players update less often, so the network
  and the game keep up
- connecting by IP only, and when it fails the window says exactly why (port closed, no answer, a local address of
  another network, the provider's CGNAT and so on)
- a mission marker only waits for players within 200 m, and `/go` starts without the rest
- up to three partners stand next to CJ in cutscenes
- every player has a colour in the chat, over the head and on the radar
- new commands `/tp`, `/tphere`, `/go`, `/mute`, `/unmute`, `/draw`
- every screen resolution in the menu, 1920x1080 included; full screen is a borderless window over the monitor by
  default, so ALT+TAB is instant (`exclusive_fullscreen = 1` in `coop\coop.ini` brings back the real full screen
  with the menu's resolution)
- the longest draw distance by default (`draw_distance` and `lod_distance` in `coop\coop.ini`, `/draw` in the game)
- everyone's map waypoints for everyone, drawn like the game's own (a black disc with a crosshair): the host's
  crosshair is red, the others' green, yellow, blue, pink
- a wounded player can be revived: it lies for 30 seconds while a partner presses `E` and gives it CPR
- the host's mission seats the partners near it in its car, or stands them on the truck next to CJ
- a lobby: friends join beforehand and press "Ready", the host presses "Start the game"; every partner picks a
  character there: CJ or a Grove Street homie
- cheats can be typed in the chat (`/code HESOYAM`), and everyone sees who entered which code and what it does
- the in-game scenes of missions (script cameras) show for the partners as for the host, and bring them next to him
- the car radio is the driver's, its passengers hear the same station; a passenger's mouse wheel picks the gun
- a new sniper scope; the more players, the sharper and more numerous the enemies
- goods at the 24/7 counter and at the bar with effects that last a while
- tank battles (`/tanks`): shells fly, armour has a front and a rear, the game's tank crews fire, soldiers carry RPGs
- ground battles (`/arena yakuza random 5`): waves of enemies in the missions' interiors and a boss at the end
- the First Person mod (BoPoH, DK22Pac, DenSpb, Crspy, Junior_Djjr; installed on its own) works with it: one can walk
  back in its first-person view, the aiming mod (imCamAim) keeps out of its way, and it no longer says "unknown game
  version"; the host can turn cheat codes off for everyone (`/cheats off`)
- the mouse in the chat: the wheel scrolls its history, the mouse picks messages, `Ctrl+C` copies them
- a "Gang war" mode in Los Santos: four gangs of players and the computer, money, attacks on territories, purchases
  and a win by the host's rules
- the original SA:COOP 0.3.0's changes: side jobs together, the other player's moves, the firewall prompt, mission
  start points and map changes of the host's missions, and its fixes; from 0.3.1: the engine's scenes with the same
  camera, the lowriders and the beach dance for two, "Home Invasion" together, a partner can start a mission, vehicles
  for the partner, a gamepad passenger (0.3.1's shared skills and the partner's own gang wars are left out: skills
  and stats are each player's own, the host starts the wars)
- Linux and macOS builds, server tests

Not compatible with the original 0.2.x versions, everyone has to update. Playing with three or more people has
barely been tested yet, so if something breaks, send `coop\coop.log` in Issues.

![The host and the partner on Grove Street](docs/images/en/street-host.jpg)

## What the mod does

### The story together

The host starts a mission, but it only begins once every partner nearby stands in the marker (or the host types
`/go`). The partner sees the same
texts, subtitles, radar blips, markers, race checkpoints and timers, and the texts come in the language of the
partner's own game. The partner also hears the lines characters say during missions outside cutscenes. Weapons
the mission gives CJ go to the partner as well. Both players get the mission reward. Enemies attack both of them,
and either player can hurt them. If the mission moves CJ somewhere else after a cutscene, the partner ends up next
to the host. The mission's limits on the wanted level apply to both players, and the gates and buildings a mission
opens or changes change for the partner too, as do the parts of the map a mission adds, such as the weed field
in "Are You Going To San Fierro?". The partner can also take the photos a mission asks for. A partner
killed during a mission comes back next to the host. If the host gets on a bike or into a vehicle with no seat
left, the same kind of vehicle appears next to the partner. The partner watches a mission's in-engine scenes
through the same camera as the host. In the lowrider contest and the beach dance the partner plays next to the
host, and their points go into the shared score. Either player can carry the crates in the house robbery with Ryder
and load them into the van.

| The host stands in the marker | The partner sees that the host is waiting |
|---|---|
| ![The host waits for the partner in the marker](docs/images/en/marker-host.jpg) | ![The partner is told that the host waits at the marker](docs/images/en/marker-partner.jpg) |

### Cutscenes

Cutscenes play for everyone at the same time, and the partners' characters (up to three) stand next to CJ in them. The
character listens to the conversation, shifts their weight, nods and looks at whoever is talking, and when CJ
walks, keeps a little behind him and to the side. These movements come from the animations of the cutscenes
themselves. A cutscene can only be skipped by everyone: one presses Enter, and the others agree with Enter or refuse
with Esc.

| | |
|---|---|
| ![Cutscene in the garage: CJ on the right, the partner's character behind him](docs/images/cutscene-garage.jpg) | ![Cutscene in the yard: the partner's character behind CJ and Ryder](docs/images/cutscene-yard.jpg) |
| ![Cutscene at the party: the partner's character by the wall on the left](docs/images/cutscene-party.jpg) | ![Cutscene with Emmet: the partner's character next to CJ](docs/images/cutscene-gun.jpg) |

### A shared world

Both players have the same pedestrians, people inside buildings, cars and police, the same time of day and the
same weather. Pedestrians chat and swear the same way for both, and police roadblocks with their barriers are seen
by both. When one player drives away, the pedestrians and cars near the other one move over to the other
player's game instead of vanishing. Knocked-down lamp posts and signs, broken glass and crates, fire, gang
territories, the building doors a mission opens and the doors of Pay 'n' Spray and mod shops look the same for
both. Horseshoes, oysters, snapshots and tags count toward the shared world. New pedestrians and cars no longer
appear right in front of the other player. Gang members from the two players' games fight each other, the police
chase whoever is wanted, and SWAT officers slide down ropes from the helicopter for both. Police officers move
the same way on both screens: they hold a pistol in both hands, crouch to shoot and sidestep around their target.
The police helicopter's gun fires on both screens, and police cars no longer vanish for the partner when the host
dies. Bodies and wrecks thrown by explosions lie in the same places for both.
Pool balls, shooting range targets and other activity objects are seen by both.

| The host's screen | The partner's screen |
|---|---|
| ![A Ballas member fights the partner, on the host's screen](docs/images/world-host.jpg) | ![The same fight on the partner's screen](docs/images/world-partner.jpg) |

### Cars

Each player drives on their own or rides as a passenger with the other. A passenger can shoot from the window: the
right mouse button leans out with the gun, the left one fires. Tuning and damage are visible to the other player.
Plane landing gear and nozzles, tank and fire truck turrets, hydraulics, the horn, the police helicopter's
searchlight, trailers, the tow truck and the airport tug all work. Explosions, rockets, grenades, tank shells,
fire truck water, NPC drive-bys, trains and RC toys are seen by both players. Either player can leave a car in the
garage at a safe house, and the host's world keeps it.

`G` takes the nearest free passenger seat, whoever's car it is. When the host's mission puts CJ in a car, the
partners within 200 m take its free seats. When it stands CJ in the back of a truck, the
partners take the cab's seats and the rest stand next to CJ in the back and shoot at the chase (`F`/`Enter` gets
off). CJ getting into a car by himself calls nobody. What the story asks of CJ stays the host's.

![The partner rides as a passenger in the host's car](docs/images/car.jpg)

### Your own progress

Food and the gym, weapon skills, clothes, haircuts, tattoos and money are separate for each player. Driving,
motorbike, cycling and flying skills are shared: both players have the better of the two. The stats in the menu are
shared too. A side job or challenge completed by one player counts for both, reward included. What a player does at a diner, the gym, a barber's or a tattoo parlour, the other player sees as
well, along with a tray of food, a cigarette, a phone at the ear, a parachute jump, a fighting style learned at the
gym, crouching with its rolls, reloading and walking in through shop doors. If a player changes their character model, for example with a skin selector, the other player sees the new
model too. The friend comes into the host's world as their own CJ, in the clothes and with the tattoos their CJ
has in their own game. If the friend joined from the main menu, the look comes from their latest save. When the
host saves the game, the friend's progress is saved on the host's side as well. The partner's school lessons and
medals, side job levels, gym, unique stunt jumps and businesses stay with the partner after the host's world is
loaded again.

| The host sees the partner | The partner sees the host |
|---|---|
| ![The partner in their own clothes on the host's screen](docs/images/look-host.jpg) | ![The host on the partner's screen](docs/images/look-partner.jpg) |

### Friendly fire

Before creating the game, the host can allow friendly fire with a checkbox in the F9 window. Then the players can
hurt and kill each other with bullets, fists, explosions, fire and cars. Without the checkbox they cannot harm
each other, and a stealth kill with a knife from behind does not work either. A player knocked down falls and gets up on both screens.

### Wounded, not dead

With a living partner within 200 m, a player whose health runs out does not die: it falls and lies there for 30
seconds, the time left shown over it. A partner comes up and presses `E`: 5 seconds of CPR, as the paramedics do,
and the wounded one gets up with all its weapons and half its health. A player can be revived twice in 6 minutes;
the third time it dies at once. Holding `E`, the wounded one gives up. In a car, in the water or with nobody near,
death is as usual. The key is `revive` in `[keys]` of `coop\coop.ini`. Lying there, you can still turn the camera
with the mouse, and the host's mission goes on until the host really dies. Only a player revives; a rival gangster
(Ballas, Vagos, Rifa) within 30 m comes and kicks the wounded one, 3 seconds off each kick.

| | |
|---|---|
| ![The host swings a baseball bat at the partner](docs/images/friendly-fire-1.jpg) | ![The partner is knocked down, and the host stands over them with the bat](docs/images/friendly-fire-2.jpg) |

### The city

The game's own 24/7s and bars sell more (`E` at the counter, then `1`-`4`); every player buys for itself, whoever
came in first:

| Goods | Price | What it does |
|---|---|---|
| A drink | $50 | for 20 minutes one of: steadier aim, quicker feet or more health; every 4 minutes a 32% chance of a minute drunk |
| Food | $25 | full health (a snack at the bar) |
| A soda | $150 | for 30 minutes: more stamina and a slightly faster run (24/7s only) |
| Cigarettes | $250 | for 30 minutes: steadier aim (24/7s only) |

The time each effect has left shows on the right of the screen.

A blown-up security van spills $1000-$10000, and they are rarer. Going into shops and houses and out is quicker.

A Grove Street homie another player's game made joins the gang of whoever called him (`G`), not the host's. When a
player goes into a building, the people and cars of its game near the other players do not vanish for them: they
go over to those players' games.

### Tanks

A Rhino's gun fires a shell that flies (250 m/s, dropping a little, up to 750 m) and bursts where it hits; it can be
seen in flight. The gun takes 4 seconds to load; the bottom of the screen shows whether it is loaded and how much
armour is left. The turret lays itself where the camera looks, allowing for the shell's drop, and a sight on the
screen marks where the shell will land (red: on a tank). Bullets and blasts still do nothing to a tank, but a shell goes through: about 18% of its strength
in the front, 30% in a side, 45% in the rear. Tanks driven by the army or the police lay their guns and fire at a
player with four wanted stars or more. Without friendly fire the players' tanks do not hurt each other.

**A tank battle**: the host types `/tanks`. Every player within 400 m of the host gets a Rhino by the road, and 12
seconds later three waves of enemy tanks come from 250-350 m away (as many as the players, one more each wave; they
drive in to 140 m and fight from there, marked red on the radar and on the screen). Soldiers with rocket launchers come on foot with each
wave (2, 3, 4): a rocket takes 30% of a tank's armour. The next wave comes only when every tank and soldier of this
one is down. All three waves destroyed: $5000 to everyone in the
battle. Everyone in it dead, or 15 minutes: lost. `/tanks join` joins it later, `/tanks stop` (the host) ends it.
While it is on there are no passers-by and no traffic for its players: they go and none come till it ends. The
enemies always know where the players are.

### Ground battles

The host types `/arena <enemies> <place> <waves>`, e.g. `/arena yakuza random 5` or `/arena ballas crack 3`
(`/arena list` lists them all). Every player is taken into an interior of the story's missions: Big Smoke's crack
palace, Madd Dogg's mansion, the meat factory, the Jefferson motel, the atrium, Liberty City, the
police station, the Sherman dam, the Kickstart stadium (`random`: any). Enemies: yakuza, ballas, vagos, aztecas, triads, mafia, rifa, bikers, police, swat, army.
1 to 10 waves, each bigger, better armed and better aimed. The next wave comes 6 seconds (30 in a big place) after
the last of this one falls; `/skip` (or `/arena skip`) starts it at once. The last wave brings a boss: Tenpenny, Pulaski, Hernandez, Big Smoke, Ryder, OG Loc, T-Bone Mendez, Jizzy B
or Mike Toreno, with an AK-47 (a gang's) or an M4 (the police's and Toreno), and with him, unannounced, his silent
men working round the players; the boss is as tough as the SWAT, no headshot kills him at once. Enemies come in squads
of 3-4 from all sides of the place, away from the players and not from where the last wave came (the mod works out
the place's floor), and never out of a wall. They know no fear and always know where the players are: out of sight
they come along the floor instead of walking into walls; in sight they fight, take cover, pop up and shoot, wait and
push at the players as a squad; one stuck works its way out. Squads have grenadiers (who throw only when the grenade
will get there). Knifemen come alone and steal up to a player's back out of its sight (waiting for it to look away),
run as CJ does and stab from behind with the game's own animation; the police's and the SWAT's knifeman is Cerberus
from Manhunt when his model (`cerberus.dff` and `cerberus.txd`) is added to `gta3.img` or to modloader. The last two
waves bring snipers: they stand far off and shoot from there, and move on when they lose sight of the players. More
armour each wave (half of it for the SWAT). The arena's doors out are shut while it is on, and the armour is gone
after it. No wanted stars in an arena or a tank battle, and weapons taken up there are not kept after it.
Weapons and body armour lie all over the place before each wave, the first one too, somewhere else each time
(better each wave; less armour where drinks machines stand).
A win pays everyone $1000 a wave, then takes everyone back where it stood. Everyone dead
or gone: lost. `/arena stop` ends it. Offline, one can play an arena alone.

### Gang war (Los Santos)

A mode of its own that the host picks when it creates the server (F9 → "Mode: Gang war: LS"). Instead of the story,
everyone starts a new game with no prologue and no missions, with Los Santos's territories as a new game has them.
In the lobby each one clicks its gang: Grove Street Families, the Ballas, Los Santos Vagos or Varrios Los Aztecas
(a gang can have several players, the first of them its leader). A gang nobody plays is left to the computer or
left out (its territories are free then), as the host sets it in the lobby with the "Bot: ON / OFF" button on each
gang's card. There the host also picks the win: every
territory, a number of them, or the most of them when the time is up. No police, ambulances or fire engines come to
an attack: near one (250 m) no wanted level is given, and one already there goes.

The players come in at their gang's base in its clothes (the leader as Sweet, Big Smoke, Ryder or Cesar) with its
weapons, and come back there after every death or arrest, not at a hospital. Their own gang's people on the street
respect them, the others hate them, and players of different gangs hurt each other. The territories are painted in
their owners' colours on the map and the radar as in the story (one under attack flashes), and the whole map is known
from the start. The streets have a little fewer people and cars than usual, and there are no doors with yellow markers (barbers, clothes shops and the rest) and no weapons lying about the city. Each
base is guarded by 5-10 of the gang's men (more with its weapons' level), a dead guard replaced after 90 seconds, and
has its own way into a house: a ring in the gang's colour at the nearest house's door (or at the base itself when no
house stands near), for the gang's own players. Every gang makes $200 of
goods every 6 minutes; the money is the gang's, and any of its players spends it in the F5 menu (or `/gang`):

| Purchase | Price | What it does |
|---|---|---|
| Attack a territory | $500 | set a target on the territory on the map and buy: 4 cars of your men drive there; the owners defend in two waves, it is yours when they are down, some of yours still stand there and no player of another gang is left in it (they have to be killed). Two attacks at most at once, by two different gangs |
| Body armour | $200 | bought once and for good: armour for the gang's players at every start and for all its men, now and later |
| Bribe the police | $550 | the police hunt the gang of the target's territory (or yours) for 5 minutes |
| Faster goods | $1000 | the goods are made faster |
| Dearer goods | $1000 ($2000 after faster goods) | the goods sell for twice as much |

The gangs' own. **Grove Street**: its first 4 attacks cost $150 (3 cars), the police bribe $700, and Emmet gives
armour for free, and once a weapons upgrade. **The Ballas**: the police bribe $350, they take protection money from the
24/7s, bars and clubs in their territories (+$50 each with the goods), but the more goods they sold, the worse their
men aim. **The Vagos**: faster goods $600, dearer goods $1000, their goods sell faster, more men but with little health
and aim. **The Aztecas**: the best shots, they come to an attack on bikes, two on each, shooting from them.

The gangs' weapons go up by levels (three for the Ballas, four for the others): from a pistol or a Desert Eagle to
the AK-47, the M4, the MP5, grenades and Molotov cocktails. Now and then a military convoy turns up in the city: the
gang whose player drives its truck to the gang's base gets the next level; the top of the screen says how far it is
(and, at its wheel, how far the base is). A gang with no territory is out. Deals
cut off, contraband, the San Fierro and Las Venturas modes and C.R.A.S.H. come later.

### Russian and English

The SA:COOP window, the chat and the hints come in Russian and English. The language switch is at the top of the
F9 window, and each player picks their own: the host can play in Russian while the partner plays in English.

| The host's screen | The partner's screen |
|---|---|
| ![The player list and the server's reply in Russian on the host's screen](docs/images/lang-host.jpg) | ![The same moment on the partner's screen, all in English](docs/images/lang-partner.jpg) |

### Everything else

Side jobs (taxi, paramedic, firefighter, vigilante, pimp), races, schools, the shooting range, trucking and the
quarry are played by each player in their own game. A side job or a race can be played together: within 20 seconds
of its start press Y, and the other players get an invitation (Y: yes, N: no; one can join). Whoever accepts sees its markers,
checkpoints, timers and hints, can ride along and help, for example put out burning cars or shoot the criminals, and
gets the same money at its end. The job's levels stay with the player who started it, and the reward for
completing the job goes to both. There is text chat and voice chat, and the farther away the
partner is, the quieter they sound. A waypoint one player sets on the map shows up on the other player's radar,
and a house one player buys appears for both. Above the partner's head you see their health, armour, weapon and
wanted stars. Turf wars near the host are the host's, and both players' kills of rival gang members provoke
them. A partner more than 100 m from the host can start a war of their own, and the host sees in the chat which
district it is for and how it ended. Cheats
for the weather, traffic, pedestrians and game speed work for both players, while weapon, money and health cheats
only work for the player who typed them. Theme cheats such as the beach party change the pedestrians for both,
but only the player who typed the code gets the outfit and the katana.

![Chat, voice and the player list on the partner's screen](docs/images/en/chat-voice.jpg)

## Installation

You need GTA San Andreas version 1.0 US, either clean or a modded build. If your game comes from Steam or the
Rockstar Games Launcher, downgrade it to 1.0 US first, for example with GTA SA Downgrader.

1. Download `SA-COOP.zip` from the [Releases](../../releases) page and unpack it into the game folder so that
   `coop.asi`, `install.bat` and the `coop` folder end up next to `gta_sa.exe`.
2. Run `install.bat`. In a clean game it installs an ASI plugin loader, and in a build that already has one it
   changes nothing.
3. Start the game. The SA:COOP window opens in the main menu, and later you can bring it up with F9. Enter your
   nickname. To switch the window to English, click EN at the top of it.

The archive also contains a detailed player guide in Russian, `Инструкция.txt`.

## How to play

The host presses F9, opens the "Host a game" tab, picks a save and clicks "Start the server". If the host is
already in the game, the server is created from the current game. The "Friendly fire" checkbox above the button
lets the players hurt each other, and "Players: up to N" sets the places (2–16). A couple of seconds later the
window shows the external IP with the port, for example `85.12.34.56:7788`, and a "Copy" button. The host sends it
to the friends. Set a password if you want to keep strangers out.

The friends open the "Join a game" tab, type that address and click "Connect". The port can go into the address
after a colon or into the "Port" field. After that, the host's world loads on their side.

![The SA:COOP window in the main menu](docs/images/en/menu.jpg)

You can also start the story from the very beginning: the last item in the save list is "New game". The host
plays the prologue alone, while the friend connects right away and waits. When CJ gets on the bike, the friend
appears next to him, and there is a bike for the friend too.

The server tries to open its UDP port on the router through UPnP, and the host's F9 window shows whether that
worked. If not, forward the port (7788/UDP by default) on the router by hand. If the provider keeps the host behind
CGNAT, the window says so too: then nobody can connect from the internet, and you need a public IP or a VPN such as
Radmin or ZeroTier (friends then connect to the VPN address).

When the host creates a game, the mod checks Windows Firewall. If it would keep friends from the server, Windows
asks to allow changes, and after "Yes" the mod allows `coop-server.exe` on every network. Without that the friend
sees "The server does not answer", even on a home network. If an antivirus removed or blocked `coop.asi` or
`coop-server.exe`, add the game folder to its exclusions and unpack the archive again.

## Keys

| Key | What it does |
|---|---|
| F9 | the SA:COOP window |
| T | chat: Enter sends a message, Esc closes the chat; the mouse wheel scrolls its history, the mouse picks messages, Ctrl+C copies them |
| B | voice chat while the key is held |
| G | take the nearest free passenger seat; by a Grove Street homie (another game's too): call him into your gang |
| E | revive a wounded partner next to you; wounded: hold to give up; by a clerk or a barman: the menu, `1`-`4` to buy; |
| Left mouse button in a tank | fire (4 s to reload) |
| Mouse wheel | as a passenger: pick the gun to shoot out of the window |
| Right mouse button | as a passenger: lean out of the window with the gun, fire with the left button |
| F2 | the player list |
| Enter / Esc | in a cutscene: offer to skip it, agree / refuse |

Chat commands:

| Command | What it does |
|---|---|
| `/help`, `/players` | the commands and the players |
| `/tp [nick]` | go to a player (no nick: when there is only one partner in the game) |
| `/tphere [nick]` | bring a player over; `/tphere all` brings everyone (host only) |
| `/go` | host: start the mission without waiting for the missing ones |
| `/mute <nick>`, `/unmute <nick>` | stop / start hearing a player's voice (for you only) |
| `/draw [0.5-4] [detail]` | draw distance (4 by default, 1 is the game's own) |
| `/tanks` | host: a tank battle; `/tanks join` joins it, `/tanks stop` ends it |
| `/arena` | host: a ground battle, `/arena yakuza random 5`; `/arena list` lists, `/arena stop` ends it |
| `/skip` | the arena: start the next wave now |
| `/gang`, F5 | the gang war: the gang's menu |
| `/cheats off`, `/cheats on` | host: turn cheat codes off or on for everyone |
| `/sync` | the sync's state: ping, the delay to each player, how many people and cars your game runs |
| `/code <cheat>` | enter a cheat, for example `/code HESOYAM` |
| `/pm <nick> <text>` | private message |
| `/kick <nick> [reason]` | host: kick a player |

A unique beginning of a nickname is enough.

## Limitations

- The server runs on the host's PC, and its upload grows with the number of players near each other: about
  8-10 Mbit/s with all 16 in one place, about 0.5 Mbit/s with four.
- A cutscene shows at most three partners next to CJ.
- Only the host can start story missions, Cesar's races and turf wars. When a mission has CJ carry something, like
  the crates in the house robbery with Ryder, the host does it.
- You see the other player's train but cannot get into it: it is driven by the player whose game made it.
- The partner gets story progress (new areas, gyms) by loading the host's world. This happens in the next
  mission's marker, or after 10 minutes without missions, and takes a couple of seconds. The partner keeps their
  position, weapons and progress.
- The host keeps the partner's profile under the partner's nickname. With a different nickname, money and skills
  start over.
- Whatever gets knocked down or broken on the streets stays that way for half an hour, then the map puts
  everything back, as in the normal game.
- The host needs an open UDP port, through UPnP or forwarded on the router. Hosting over mobile internet or behind
  CGNAT does not work without a VPN.
- Only `gta_sa.exe` version 1.0 US is supported. The players' builds may differ, but mods that change the story or
  the saves are needed on both sides. If the partner's `data\script\main.scm` differs from the host's, the host's
  world does not load for them, and the mod says so in the chat.
- Mods installed through modloader work. Each player sees their own models, textures and effects. A car that a mod
  adds under a new ID is only visible to the player who has that mod.

## Bugs and ideas

Report bugs, crashes and ideas in [Issues](../../issues/new/choose). A short form opens there: what happened, how
to make it happen again, the mod version and which game you have. The form is in Russian, but you can answer in
English. A [Telegram group](https://t.me/sacoop_forum) is also available if you encounter difficulties sending to GitHub.

With a bug report, attach `coop\coop.log` from the game folder of the player who had the problem. If the game
crashed, also attach `coop\crash.log`, and if you cannot connect, attach the host's `coop\server-data\server.log`.
The log of the previous run is kept as `coop\coop.old.log`: send it if the game was started again after a crash.
When something is out of step between the players, type `/sync` in the game on both sides at that moment: the lines
go to the chat and to the log.

## Building from source

You need Windows, Python 3.10 or newer and PowerShell. A script downloads the compiler (llvm-mingw) into the
`toolchain` folder:

```
powershell -ExecutionPolicy Bypass -File tools/get-toolchain.ps1
python tools/build.py          # coop.dll, coop-server.exe and vorbisFile.dll in build/release/bin
python tools/build.py dist     # the same plus the ready archive dist/SA-COOP.zip
python tools/fsd_check.py      # checks the dependencies between the layers
python tools/i18n_check.py     # checks that every text of the game has its English one
python tools/build.py servertest  # server tests: build/release/bin/coop-server-test.exe
```

On Linux and macOS set `LLVM_MINGW` to the llvm-mingw folder; the server tests run under Wine:
`LLVM_MINGW=~/llvm-mingw python3 tools/build.py && wine build/release/bin/coop-server-test.exe`.

GitHub Actions builds every push and keeps the archive in the build artifacts. A tag like `v0.2.0` publishes a
release with `SA-COOP.zip` attached.

## Project layout

The sources follow Feature-Sliced Design. Each of the three programs is a separate application with the layers
`app`, `pages`, `widgets`, `features`, `entities` and `shared`, and the code they share lives in `src/common`.

```
src/
├── client/        coop.asi, runs inside the game
│   ├── app/       entry point, main loop, game patches
│   ├── pages/     the F9 window
│   ├── widgets/   the overlay on top of the game
│   ├── features/  story and cutscenes, progress, garages, voice, weather and more
│   ├── entities/  session, player, vehicle, ped, mission, profile
│   └── shared/    engine wrappers, network, drawing and input, config
├── server/        coop-server.exe, the server for 2–16 players
├── loader/        vorbisFile.dll, the ASI loader for a clean game
└── common/        protocol, network, log and utilities for all three
```

A layer only includes the layers below it and its own slice, which `tools/fsd_check.py` checks. How the mod works
inside and why it is built this way is described in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) (in Russian).

## Third-party code

- [ENet](http://enet.bespin.org) (MIT license): network transport, `third_party/enet`.
- [MinHook](https://github.com/TsudaKageyu/minhook) (BSD 2-Clause license): function hooking, `third_party/minhook`.
- [llvm-mingw](https://github.com/mstorsjo/llvm-mingw): the compiler, downloaded during the build.
- Engine addresses and structures were checked against [gta-reversed](https://github.com/gta-reversed/gta-reversed)
  and [plugin-sdk](https://github.com/DK22Pac/plugin-sdk).

## License

The SA:COOP code is released under the MIT license (see [LICENSE](LICENSE)): the original project © Karimov-Akbar,
the fork's changes © Farus. ENet and MinHook have their own
licenses, and their texts are in `third_party` and in the `coop\licenses` folder of the archive.

SA:COOP is not affiliated with Rockstar Games or Take-Two Interactive. You need your own copy of GTA San Andreas,
and the repository contains no game files.

## Support the author of the original SA:COOP

<p align="left">
  <a href="https://dalink.to/itscavalier"><img src="https://img.shields.io/badge/DonationAlerts-F57507?style=for-the-badge" alt="Support"></a>
</p>
