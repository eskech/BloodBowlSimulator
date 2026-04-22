#pragma once
#include <algorithm>
#include <array>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Pitch zones (from the perspective of the team currently on offense)
// ---------------------------------------------------------------------------
enum class Zone : int {
    OwnEndZone  = 0,  // columns 1-2
    OwnHalf     = 1,  // columns 3-13
    Midfield    = 2,  // column  13-14 (overlap centre)
    OppHalf     = 3,  // columns 14-24
    OppEndZone  = 4   // columns 25-26
};

// Arithmetic helpers – avoids repeated static_cast at call sites.
inline Zone operator+(Zone z, int d) { return static_cast<Zone>(static_cast<int>(z) + d); }
inline Zone operator-(Zone z, int d) { return static_cast<Zone>(static_cast<int>(z) - d); }
inline int  operator-(Zone a, Zone b) { return static_cast<int>(a) - static_cast<int>(b); }

// ---------------------------------------------------------------------------
// Skill helpers
// ---------------------------------------------------------------------------
using Skills = std::vector<std::string>;

inline bool hasSkill(const Skills& skills, std::string_view skill) {
    return std::ranges::any_of(skills, [&](const auto& s) { return s == skill; });
}

// ---------------------------------------------------------------------------
// Skill bitmask — O(1) hot-path lookup.
// Each constant is a bit index (0-173).  PlayerStats stores
// std::array<uint64_t,3> skillFlags; has(idx) tests the right word.
// 174 skills fit in 3×64 = 192 bits.
// ---------------------------------------------------------------------------
namespace SK {
    // ── Simulated (bits 0-17) ──────────────────────────────────────────────
    inline constexpr int Dodge              =   0;  // Dodge
    inline constexpr int Block              =   1;  // Block
    inline constexpr int Wrestle            =   2;  // Wrestle
    inline constexpr int SureHands          =   3;  // Sure Hands
    inline constexpr int Catch              =   4;  // Catch
    inline constexpr int Pass               =   5;  // Pass
    inline constexpr int Accurate           =   6;  // Accurate
    inline constexpr int DivingCatch        =   7;  // Diving Catch
    inline constexpr int DivingTackle       =   8;  // Diving Tackle
    inline constexpr int Tackle             =   9;  // Tackle
    inline constexpr int MightyBlow         =  10;  // Mighty Blow
    inline constexpr int Claws              =  11;  // Claws
    inline constexpr int StandFirm          =  12;  // Stand Firm
    inline constexpr int Sprint             =  13;  // Sprint
    inline constexpr int SureFeet           =  14;  // Sure Feet
    inline constexpr int NervesOfSteel      =  15;  // Nerves of Steel
    inline constexpr int Pro                =  16;  // Pro
    inline constexpr int Guard              =  17;  // Guard
    // ── Full roster (bits 18-173) ──────────────────────────────────────────
    inline constexpr int ASneakyPair        =  18;  // A Sneaky Pair
    inline constexpr int AllYouCanEat       =  19;  // All You Can Eat
    inline constexpr int AlwaysHungry       =  20;  // Always Hungry
    inline constexpr int AnimalSavagery     =  21;  // Animal Savagery
    inline constexpr int Animosity          =  22;  // Animosity
    inline constexpr int ArmBar             =  23;  // Arm Bar
    inline constexpr int BalefulHex         =  24;  // Baleful Hex
    inline constexpr int BallAndChain       =  25;  // Ball and Chain
    inline constexpr int BeerBarrelBash     =  26;  // Beer Barrel Bash!
    inline constexpr int BigHand            =  27;  // Big Hand
    inline constexpr int BlackInk           =  28;  // Black Ink
    inline constexpr int BlastIt            =  29;  // Blast It!
    inline constexpr int BlastinSolvesEverything = 30; // Blastin' Solves Everything
    inline constexpr int BlindRage          =  31;  // Blind Rage
    inline constexpr int Bloodlust          =  32;  // Bloodlust
    inline constexpr int Bombardier         =  33;  // Bombardier
    inline constexpr int BoneHead           =  34;  // Bone Head
    inline constexpr int Brawler            =  35;  // Brawler
    inline constexpr int BreakTackle        =  36;  // Break Tackle
    inline constexpr int BreatheFire        =  37;  // Breathe Fire
    inline constexpr int Bullseye           =  38;  // Bullseye
    inline constexpr int Cannoneer          =  39;  // Cannoneer
    inline constexpr int CatchOfTheDay      =  40;  // Catch of the Day
    inline constexpr int Chainsaw           =  41;  // Chainsaw
    inline constexpr int CloudBurster       =  42;  // Cloud Burster
    inline constexpr int ConsummateProfessional = 43; // Consummate Professional
    inline constexpr int CrushingBlow       =  44;  // Crushing Blow
    inline constexpr int Dauntless          =  45;  // Dauntless
    inline constexpr int Decay              =  46;  // Decay
    inline constexpr int Defensive          =  47;  // Defensive
    inline constexpr int DirtyPlayer        =  48;  // Dirty Player
    inline constexpr int DisturbingPresence =  49;  // Disturbing Presence
    inline constexpr int Drunkard           =  50;  // Drunkard
    inline constexpr int Dumpoff            =  51;  // Dump-off
    inline constexpr int DwarfenGrit        =  52;  // Dwarfen Grit
    inline constexpr int DwarfenScourge     =  53;  // Dwarfen Scourge
    inline constexpr int ExcuseMeAreYouAZoat = 54;  // Excuse Me, Are You a Zoat?
    inline constexpr int ExtraArms          =  55;  // Extra Arms
    inline constexpr int EyeGouge           =  56;  // Eye Gouge
    inline constexpr int Fend               =  57;  // Fend
    inline constexpr int FoulAppearance     =  58;  // Foul Appearance
    inline constexpr int FrenziedRush       =  59;  // Frenzied Rush
    inline constexpr int Frenzy             =  60;  // Frenzy
    inline constexpr int Fumblerooski       =  61;  // Fumblerooski
    inline constexpr int FuriousOutburst    =  62;  // Furious Outburst
    inline constexpr int FuryOfTheBloodGod  =  63;  // Fury of the Blood God
    inline constexpr int GiveAndGo          =  64;  // Give and Go
    inline constexpr int GoredByTheBull     =  65;  // Gored by the Bull
    inline constexpr int Grab               =  66;  // Grab
    inline constexpr int HailMaryPass       =  67;  // Hail Mary Pass
    inline constexpr int HalflingLuck       =  68;  // Halfling Luck
    inline constexpr int Hatred             =  69;  // Hatred
    inline constexpr int HitAndRun          =  70;  // Hit and Run
    inline constexpr int Horns              =  71;  // Horns
    inline constexpr int HypnoticGaze       =  72;  // Hypnotic Gaze
    inline constexpr int IllBeBack          =  73;  // I'll Be Back!
    inline constexpr int IllCarryYou        =  74;  // I'll Carry You
    inline constexpr int Incorporeal        =  75;  // Incorporeal
    inline constexpr int Indomitable        =  76;  // Indomitable
    inline constexpr int Insignificant      =  77;  // Insignificant
    inline constexpr int IronHardSkin       =  78;  // Iron Hard Skin
    inline constexpr int Juggernaut         =  79;  // Juggernaut
    inline constexpr int JumpUp             =  80;  // Jump Up
    inline constexpr int Kaboom             =  81;  // Kaboom!
    inline constexpr int Kick               =  82;  // Kick
    inline constexpr int KickEmWhileTheyreDown = 83; // Kick 'em While They're Down!
    inline constexpr int KickTeammate       =  84;  // Kick Team-mate
    inline constexpr int KrumpAndSmash      =  85;  // Krump and Smash
    inline constexpr int Leader             =  86;  // Leader
    inline constexpr int Leap               =  87;  // Leap
    inline constexpr int LethalFlight       =  88;  // Lethal Flight
    inline constexpr int LoneFouler         =  89;  // Lone Fouler
    inline constexpr int Loner              =  90;  // Loner
    inline constexpr int LookIntoMyEyes     =  91;  // Look into my Eyes
    inline constexpr int LordOfChaos        =  92;  // Lord of Chaos
    inline constexpr int MasterAssassin     =  93;  // Master Assassin
    inline constexpr int MaximumCarnage     =  94;  // Maximum Carnage
    inline constexpr int MesmerizingGaze    =  95;  // Mesmerizing Gaze
    inline constexpr int MonstrousMouth     =  96;  // Monstrous Mouth
    inline constexpr int MultipleBlock      =  97;  // Multiple Block
    inline constexpr int MyBall             =  98;  // My Ball
    inline constexpr int NoBall             =  99;  // No Ball
    inline constexpr int OldPro             = 100;  // Old Pro
    inline constexpr int OnTheBall          = 101;  // On the Ball
    inline constexpr int PassBlock          = 102;  // Pass Block
    inline constexpr int Pickmeup           = 103;  // Pick-me-up
    inline constexpr int PileDriver         = 104;  // Pile Driver
    inline constexpr int PilingOn           = 105;  // Piling On
    inline constexpr int PlagueRidden       = 106;  // Plague Ridden
    inline constexpr int PogoStick          = 107;  // Pogo Stick
    inline constexpr int PrehensileTail     = 108;  // Prehensile Tail
    inline constexpr int PrimalSavagery     = 109;  // Primal Savagery
    inline constexpr int ProjectileVomit    = 110;  // Projectile Vomit
    inline constexpr int PumpUpTheCrowd     = 111;  // Pump Up the Crowd
    inline constexpr int Punt               = 112;  // Punt
    inline constexpr int PutTheBootIn       = 113;  // Put the Boot In
    inline constexpr int PutridRegurgitation = 114; // Putrid Regurgitation
    inline constexpr int QuickBite          = 115;  // Quick Bite
    inline constexpr int QuickFoul          = 116;  // Quick Foul
    inline constexpr int RaidingParty       = 117;  // Raiding Party
    inline constexpr int Ram                = 118;  // Ram
    inline constexpr int ReallyStupid       = 119;  // Really Stupid
    inline constexpr int Regeneration       = 120;  // Regeneration
    inline constexpr int Reliable           = 121;  // Reliable
    inline constexpr int RightStuff         = 122;  // Right Stuff
    inline constexpr int Saboteur           = 123;  // Saboteur
    inline constexpr int SafePairOfHands    = 124;  // Safe Pair of Hands
    inline constexpr int SafePass           = 125;  // Safe Pass
    inline constexpr int SavageBlow         = 126;  // Savage Blow
    inline constexpr int SavageMauling      = 127;  // Savage Mauling
    inline constexpr int SecretWeapon       = 128;  // Secret Weapon
    inline constexpr int Shadowing          = 129;  // Shadowing
    inline constexpr int ShotToNothing      = 130;  // Shot to Nothing
    inline constexpr int Sidestep           = 131;  // Sidestep
    inline constexpr int SlashingNails      = 132;  // Slashing Nails
    inline constexpr int Slayer             = 133;  // Slayer
    inline constexpr int SneakiestOfTheLot  = 134;  // Sneakiest of the Lot
    inline constexpr int SneakyGit          = 135;  // Sneaky Git
    inline constexpr int Stab               = 136;  // Stab
    inline constexpr int Stakes             = 137;  // Stakes
    inline constexpr int StarOfTheShow      = 138;  // Star of the Show
    inline constexpr int SteadyFooting      = 139;  // Steady Footing
    inline constexpr int StripBall          = 140;  // Strip Ball
    inline constexpr int StrongArm          = 141;  // Strong Arm
    inline constexpr int StrongPassingGame  = 142;  // Strong Passing Game
    inline constexpr int Stunty             = 143;  // Stunty
    inline constexpr int SwiftAsTheBreeze   = 144;  // Swift as the Breeze
    inline constexpr int Swoop              = 145;  // Swoop
    inline constexpr int TakeRoot           = 146;  // Take Root
    inline constexpr int TastyMorsel        = 147;  // Tasty Morsel
    inline constexpr int Taunt              = 148;  // Taunt
    inline constexpr int Tentacles          = 149;  // Tentacles
    inline constexpr int TheBallista        = 150;  // The Ballista
    inline constexpr int TheFlashingBlade   = 151;  // The Flashing Blade
    inline constexpr int ThickSkull         = 152;  // Thick Skull
    inline constexpr int ThinkingMansTroll  = 153;  // Thinking Man's Troll
    inline constexpr int ThrowTeammate      = 154;  // Throw Team-mate
    inline constexpr int Timmmber           = 155;  // Timmm-ber!
    inline constexpr int Titchy             = 156;  // Titchy
    inline constexpr int ToxinConnoisseur   = 157;  // Toxin Connoisseur
    inline constexpr int Treacherous        = 158;  // Treacherous
    inline constexpr int Trickster          = 159;  // Trickster
    inline constexpr int TwoHeads           = 160;  // Two Heads
    inline constexpr int UnchannelledFury   = 161;  // Unchannelled Fury
    inline constexpr int Unsteady           = 162;  // Unsteady
    inline constexpr int UnstoppableMomentum = 163; // Unstoppable Momentum
    inline constexpr int VeryLongLegs       = 164;  // Very Long Legs
    inline constexpr int ViciousVines       = 165;  // Vicious Vines
    inline constexpr int ViolentInnovator   = 166;  // Violent Innovator
    inline constexpr int WatchOut           = 167;  // Watch Out!
    inline constexpr int WeepingDagger      = 168;  // Weeping Dagger
    inline constexpr int WhirlingDervish    = 169;  // Whirling Dervish
    inline constexpr int WisdomOfTheWhiteDwarf = 170; // Wisdom of the White Dwarf
    inline constexpr int WoodlandFury       = 171;  // Woodland Fury
    inline constexpr int WorkingInTandem    = 172;  // Working In Tandem
    inline constexpr int Yoink              = 173;  // Yoink!
    inline constexpr int Swarming          = 174;  // Swarming
}  // namespace SK

// Returns the bit index for a skill name, or -1 if unknown.
inline int skillNameToIndex(std::string_view s) {
    if (s == "Dodge")                          return SK::Dodge;
    if (s == "Block")                          return SK::Block;
    if (s == "Wrestle")                        return SK::Wrestle;
    if (s == "Sure Hands")                     return SK::SureHands;
    if (s == "Catch")                          return SK::Catch;
    if (s == "Pass")                           return SK::Pass;
    if (s == "Accurate")                       return SK::Accurate;
    if (s == "Diving Catch")                   return SK::DivingCatch;
    if (s == "Diving Tackle")                  return SK::DivingTackle;
    if (s == "Tackle")                         return SK::Tackle;
    if (s == "Mighty Blow")                    return SK::MightyBlow;
    if (s == "Claws")                          return SK::Claws;
    if (s == "Stand Firm")                     return SK::StandFirm;
    if (s == "Sprint")                         return SK::Sprint;
    if (s == "Sure Feet")                      return SK::SureFeet;
    if (s == "Nerves of Steel")                return SK::NervesOfSteel;
    if (s == "Pro")                            return SK::Pro;
    if (s == "Guard")                          return SK::Guard;
    if (s == "A Sneaky Pair")                  return SK::ASneakyPair;
    if (s == "All You Can Eat")                return SK::AllYouCanEat;
    if (s == "Always Hungry")                  return SK::AlwaysHungry;
    if (s == "Animal Savagery")                return SK::AnimalSavagery;
    if (s == "Animosity")                      return SK::Animosity;
    if (s == "Arm Bar")                        return SK::ArmBar;
    if (s == "Baleful Hex")                    return SK::BalefulHex;
    if (s == "Ball and Chain")                 return SK::BallAndChain;
    if (s == "Beer Barrel Bash!")              return SK::BeerBarrelBash;
    if (s == "Big Hand")                       return SK::BigHand;
    if (s == "Black Ink")                      return SK::BlackInk;
    if (s == "Blast It!")                      return SK::BlastIt;
    if (s == "Blastin' Solves Everything")     return SK::BlastinSolvesEverything;
    if (s == "Blind Rage")                     return SK::BlindRage;
    if (s == "Bloodlust")                      return SK::Bloodlust;
    if (s == "Bombardier")                     return SK::Bombardier;
    if (s == "Bone Head")                      return SK::BoneHead;
    if (s == "Brawler")                        return SK::Brawler;
    if (s == "Break Tackle")                   return SK::BreakTackle;
    if (s == "Breathe Fire")                   return SK::BreatheFire;
    if (s == "Bullseye")                       return SK::Bullseye;
    if (s == "Cannoneer")                      return SK::Cannoneer;
    if (s == "Catch of the Day")               return SK::CatchOfTheDay;
    if (s == "Chainsaw")                       return SK::Chainsaw;
    if (s == "Cloud Burster")                  return SK::CloudBurster;
    if (s == "Consummate Professional")        return SK::ConsummateProfessional;
    if (s == "Crushing Blow")                  return SK::CrushingBlow;
    if (s == "Dauntless")                      return SK::Dauntless;
    if (s == "Decay")                          return SK::Decay;
    if (s == "Defensive")                      return SK::Defensive;
    if (s == "Dirty Player")                   return SK::DirtyPlayer;
    if (s == "Disturbing Presence")            return SK::DisturbingPresence;
    if (s == "Drunkard")                       return SK::Drunkard;
    if (s == "Dump-off")                       return SK::Dumpoff;
    if (s == "Dwarfen Grit")                   return SK::DwarfenGrit;
    if (s == "Dwarfen Scourge")                return SK::DwarfenScourge;
    if (s == "Excuse Me, Are You a Zoat?")     return SK::ExcuseMeAreYouAZoat;
    if (s == "Extra Arms")                     return SK::ExtraArms;
    if (s == "Eye Gouge")                      return SK::EyeGouge;
    if (s == "Fend")                           return SK::Fend;
    if (s == "Foul Appearance")                return SK::FoulAppearance;
    if (s == "Frenzied Rush")                  return SK::FrenziedRush;
    if (s == "Frenzy")                         return SK::Frenzy;
    if (s == "Fumblerooski")                   return SK::Fumblerooski;
    if (s == "Furious Outburst")               return SK::FuriousOutburst;
    if (s == "Fury of the Blood God")          return SK::FuryOfTheBloodGod;
    if (s == "Give and Go")                    return SK::GiveAndGo;
    if (s == "Gored by the Bull")              return SK::GoredByTheBull;
    if (s == "Grab")                           return SK::Grab;
    if (s == "Hail Mary Pass")                 return SK::HailMaryPass;
    if (s == "Halfling Luck")                  return SK::HalflingLuck;
    if (s == "Hatred")                         return SK::Hatred;
    if (s == "Hit and Run")                    return SK::HitAndRun;
    if (s == "Horns")                          return SK::Horns;
    if (s == "Hypnotic Gaze")                  return SK::HypnoticGaze;
    if (s == "I'll Be Back!")                  return SK::IllBeBack;
    if (s == "I'll Carry You")                 return SK::IllCarryYou;
    if (s == "Incorporeal")                    return SK::Incorporeal;
    if (s == "Indomitable")                    return SK::Indomitable;
    if (s == "Insignificant")                  return SK::Insignificant;
    if (s == "Iron Hard Skin")                 return SK::IronHardSkin;
    if (s == "Juggernaut")                     return SK::Juggernaut;
    if (s == "Jump Up")                        return SK::JumpUp;
    if (s == "Kaboom!")                        return SK::Kaboom;
    if (s == "Kick")                           return SK::Kick;
    if (s == "Kick 'em While They're Down!")   return SK::KickEmWhileTheyreDown;
    if (s == "Kick Team-mate")                 return SK::KickTeammate;
    if (s == "Krump and Smash")                return SK::KrumpAndSmash;
    if (s == "Leader")                         return SK::Leader;
    if (s == "Leap")                           return SK::Leap;
    if (s == "Lethal Flight")                  return SK::LethalFlight;
    if (s == "Lone Fouler")                    return SK::LoneFouler;
    if (s == "Loner")                          return SK::Loner;
    if (s == "Loner (4+)")                     return SK::Loner;
    if (s == "Loner (3+)")                     return SK::Loner;
    if (s == "Look into my Eyes")              return SK::LookIntoMyEyes;
    if (s == "Lord of Chaos")                  return SK::LordOfChaos;
    if (s == "Master Assassin")                return SK::MasterAssassin;
    if (s == "Maximum Carnage")                return SK::MaximumCarnage;
    if (s == "Mesmerizing Gaze")               return SK::MesmerizingGaze;
    if (s == "Monstrous Mouth")                return SK::MonstrousMouth;
    if (s == "Multiple Block")                 return SK::MultipleBlock;
    if (s == "My Ball")                        return SK::MyBall;
    if (s == "No Ball")                        return SK::NoBall;
    if (s == "Old Pro")                        return SK::OldPro;
    if (s == "On the Ball")                    return SK::OnTheBall;
    if (s == "Pass Block")                     return SK::PassBlock;
    if (s == "Pick-me-up")                     return SK::Pickmeup;
    if (s == "Pile Driver")                    return SK::PileDriver;
    if (s == "Piling On")                      return SK::PilingOn;
    if (s == "Plague Ridden")                  return SK::PlagueRidden;
    if (s == "Pogo Stick")                     return SK::PogoStick;
    if (s == "Prehensile Tail")                return SK::PrehensileTail;
    if (s == "Primal Savagery")                return SK::PrimalSavagery;
    if (s == "Projectile Vomit")               return SK::ProjectileVomit;
    if (s == "Pump Up the Crowd")              return SK::PumpUpTheCrowd;
    if (s == "Punt")                           return SK::Punt;
    if (s == "Put the Boot In")                return SK::PutTheBootIn;
    if (s == "Putrid Regurgitation")           return SK::PutridRegurgitation;
    if (s == "Quick Bite")                     return SK::QuickBite;
    if (s == "Quick Foul")                     return SK::QuickFoul;
    if (s == "Raiding Party")                  return SK::RaidingParty;
    if (s == "Ram")                            return SK::Ram;
    if (s == "Really Stupid")                  return SK::ReallyStupid;
    if (s == "Regeneration")                   return SK::Regeneration;
    if (s == "Reliable")                       return SK::Reliable;
    if (s == "Right Stuff")                    return SK::RightStuff;
    if (s == "Saboteur")                       return SK::Saboteur;
    if (s == "Safe Pair of Hands")             return SK::SafePairOfHands;
    if (s == "Safe Pass")                      return SK::SafePass;
    if (s == "Savage Blow")                    return SK::SavageBlow;
    if (s == "Savage Mauling")                 return SK::SavageMauling;
    if (s == "Secret Weapon")                  return SK::SecretWeapon;
    if (s == "Shadowing")                      return SK::Shadowing;
    if (s == "Shot to Nothing")                return SK::ShotToNothing;
    if (s == "Sidestep")                       return SK::Sidestep;
    if (s == "Slashing Nails")                 return SK::SlashingNails;
    if (s == "Slayer")                         return SK::Slayer;
    if (s == "Sneakiest of the Lot")           return SK::SneakiestOfTheLot;
    if (s == "Sneaky Git")                     return SK::SneakyGit;
    if (s == "Stab")                           return SK::Stab;
    if (s == "Stakes")                         return SK::Stakes;
    if (s == "Star of the Show")               return SK::StarOfTheShow;
    if (s == "Steady Footing")                 return SK::SteadyFooting;
    if (s == "Strip Ball")                     return SK::StripBall;
    if (s == "Strong Arm")                     return SK::StrongArm;
    if (s == "Strong Passing Game")            return SK::StrongPassingGame;
    if (s == "Stunty")                         return SK::Stunty;
    if (s == "Swift as the Breeze")            return SK::SwiftAsTheBreeze;
    if (s == "Swoop")                          return SK::Swoop;
    if (s == "Take Root")                      return SK::TakeRoot;
    if (s == "Tasty Morsel")                   return SK::TastyMorsel;
    if (s == "Taunt")                          return SK::Taunt;
    if (s == "Tentacles")                      return SK::Tentacles;
    if (s == "The Ballista")                   return SK::TheBallista;
    if (s == "The Flashing Blade")             return SK::TheFlashingBlade;
    if (s == "Thick Skull")                    return SK::ThickSkull;
    if (s == "Thinking Man's Troll")           return SK::ThinkingMansTroll;
    if (s == "Throw Team-mate")                return SK::ThrowTeammate;
    if (s == "Timmm-ber!")                     return SK::Timmmber;
    if (s == "Titchy")                         return SK::Titchy;
    if (s == "Toxin Connoisseur")              return SK::ToxinConnoisseur;
    if (s == "Treacherous")                    return SK::Treacherous;
    if (s == "Trickster")                      return SK::Trickster;
    if (s == "Two Heads")                      return SK::TwoHeads;
    if (s == "Unchannelled Fury")              return SK::UnchannelledFury;
    if (s == "Unsteady")                       return SK::Unsteady;
    if (s == "Unstoppable Momentum")           return SK::UnstoppableMomentum;
    if (s == "Very Long Legs")                 return SK::VeryLongLegs;
    if (s == "Vicious Vines")                  return SK::ViciousVines;
    if (s == "Violent Innovator")              return SK::ViolentInnovator;
    if (s == "Watch Out!")                     return SK::WatchOut;
    if (s == "Weeping Dagger")                 return SK::WeepingDagger;
    if (s == "Whirling Dervish")               return SK::WhirlingDervish;
    if (s == "Wisdom of the White Dwarf")      return SK::WisdomOfTheWhiteDwarf;
    if (s == "Woodland Fury")                  return SK::WoodlandFury;
    if (s == "Working In Tandem")              return SK::WorkingInTandem;
    if (s == "Yoink!")                         return SK::Yoink;
    if (s == "Swarming")                       return SK::Swarming;
    return -1;
}

// ---------------------------------------------------------------------------
// Per-player strategy: probability (0.0 – 1.0) of using each optional skill.
// Team-level defaults are merged with per-player overrides at load time.
// ---------------------------------------------------------------------------
struct PlayerStrategy {
    // Wrestle – when Both Down is the block result, choose to have both fall.
    // Useful when you want to neutralise both players; avoided when carrying ball.
    float wrestle{0.8f};

    // Stand Firm – when pushed, remain in current zone instead of moving back.
    // Useful for holding the line; irrelevant without the Stand Firm skill.
    float standFirm{0.7f};

    // Diving Tackle – after an opponent successfully dodges away from this player,
    // fall prone to force the opponent to make an additional dodge roll.
    float divingTackle{0.6f};

    // Pro – when a roll fails, attempt a Pro re-roll (succeeds on 3+, d6).
    // Burn the skill sparingly; 0.5 = use it on roughly half of failures.
    float pro{0.5f};
};

// ── Per-game weather context ──────────────────────────────────────────────────
// Rolled once at the start of each game (2D6 table) and threaded through all
// turn functions that involve passing, catching, or pickup rolls.
struct GameContext {
    int  paModifier{0};       // +1 = harder to pass (Very Sunny, Pouring Rain)
    int  catchModifier{0};    // +1 = harder to catch/pick up (Pouring Rain)
    bool blizzard{false};     // no passing allowed at all (Blizzard)
    bool swelteringHeat{false}; // random KO check before each drive
};

// Merge: player overrides take precedence over team defaults where explicitly set.
// We represent "not overridden" as a negative sentinel in parsing.
struct StrategyOverride {
    float wrestle{-1.f};
    float standFirm{-1.f};
    float divingTackle{-1.f};
    float pro{-1.f};

    PlayerStrategy mergedWith(const PlayerStrategy& base) const {
        PlayerStrategy r = base;
        if (wrestle      >= 0.f) r.wrestle      = wrestle;
        if (standFirm    >= 0.f) r.standFirm    = standFirm;
        if (divingTackle >= 0.f) r.divingTackle = divingTackle;
        if (pro          >= 0.f) r.pro          = pro;
        return r;
    }
};

// ---------------------------------------------------------------------------
// Seed-data structures (loaded from bloodbowl-2025-seed.json)
// ---------------------------------------------------------------------------
struct RosterPosition {
    std::string name;
    int cost{};
    int maxCount{};
    int ma{6}, st{3}, ag{3};
    std::optional<int> pa{4};  // null for players who can't pass
    int av{8};
    Skills startingSkills;
    Skills normalSkillAccess;
    Skills doubleSkillAccess;
    std::string keywords;
};

struct Race {
    std::string name;
    int tier{};
    int rerollCost{};
    bool canHaveApothecary{};
    bool canHaveTeamCaptain{};
    std::vector<RosterPosition> positions;

    const RosterPosition* findPosition(std::string_view posName) const {
        for (const auto& p : positions)
            if (p.name == posName) return &p;
        return nullptr;
    }
};

// ---------------------------------------------------------------------------
// Star player entry (from seed starPlayers array)
// ---------------------------------------------------------------------------
struct StarPlayer {
    std::string name;
    int cost{};
    int ma{6}, st{3}, ag{3};
    std::optional<int> pa;
    int av{8};
    Skills skills;
    std::vector<std::string> allowedTeams;  // empty → may be hired by any team
};

struct SeedData {
    std::vector<Race> races;
    std::vector<StarPlayer> starPlayers;
    std::vector<std::string> skillNames;
    // skill name → category (e.g. "Block" → "General")
    std::unordered_map<std::string, std::string> skillCategories;

    const Race* findRace(std::string_view name) const {
        for (const auto& r : races)
            if (r.name == name) return &r;
        return nullptr;
    }

    const StarPlayer* findStarPlayer(std::string_view name) const {
        for (const auto& sp : starPlayers)
            if (sp.name == name) return &sp;
        return nullptr;
    }

    // Returns an error string if the skill cannot be added to this position.
    // Trait and Special skills can never be added via leveling.
    std::expected<void, std::string>
    validateExtraSkill(const RosterPosition& pos, const std::string& skillName) const {
        auto it = skillCategories.find(skillName);
        if (it == skillCategories.end())
            return std::unexpected(std::format("Unknown skill: '{}'", skillName));

        const std::string& cat = it->second;

        // Trait and Special skills are innate – cannot be acquired through leveling
        if (cat == "Trait" || cat == "Special")
            return std::unexpected(
                std::format("Skill '{}' ({}) cannot be gained through leveling", skillName, cat));

        bool canAccess = hasSkill(pos.normalSkillAccess, cat)
                      || hasSkill(pos.doubleSkillAccess, cat);
        if (!canAccess)
            return std::unexpected(
                std::format("Position '{}' cannot access {} skill '{}' "
                            "(allowed: normal={}, double={})",
                            pos.name, cat, skillName,
                            [&]() {
                                std::string s;
                                for (const auto& a : pos.normalSkillAccess)
                                    s += a + " ";
                                return s;
                            }(),
                            [&]() {
                                std::string s;
                                for (const auto& a : pos.doubleSkillAccess)
                                    s += a + " ";
                                return s;
                            }()));
        return {};
    }
};

// ---------------------------------------------------------------------------
// Match-config structures (loaded from the user's match JSON)
// ---------------------------------------------------------------------------
struct PlayerConfig {
    std::string name;
    std::string position;
    Skills extraSkills;           // skills added beyond position starting skills
    StrategyOverride strategy;    // per-player strategy overrides (optional)
    bool isTeamCaptain{false};    // gains Pro; free team re-roll on natural 6
};

struct TeamConfig {
    std::string name;
    std::string race;
    int rerolls{2};
    bool hasApothecary{false};
    bool riotousRookies{false};
    PlayerStrategy defaultStrategy;   // team-wide defaults
    std::vector<PlayerConfig> players;
    std::vector<std::string> starPlayers;  // names from seed starPlayers catalogue
};

struct MatchConfig {
    TeamConfig team1, team2;
    int simulations{10'000};
};

// ---------------------------------------------------------------------------
// Tournament configuration (loaded from a tournament JSON file)
// ---------------------------------------------------------------------------
struct TournamentConfig {
    int         numTournaments{1000};   // how many times to run the whole bracket
    int         numRounds{5};           // Swiss rounds per tournament
    int         matchGames{1};          // games per head-to-head match
    std::string pairingSystem{"dutch"}; // "dutch" or "monrad"
    std::vector<TeamConfig> teams;
};

// ---------------------------------------------------------------------------
// In-game player stats (resolved from RosterPosition + extra skills)
// ---------------------------------------------------------------------------
struct PlayerStats {
    std::string name;
    std::string position;
    int ma{6}, st{3}, ag{3};
    std::optional<int> pa{4};
    int av{8};
    std::array<uint64_t, 3> skillFlags{};

    bool has(int idx) const {
        return idx >= 0 && (skillFlags[static_cast<unsigned>(idx) >> 6]
                            >> (idx & 63)) & 1;
    }
};

// ---------------------------------------------------------------------------
// In-game player state
// ---------------------------------------------------------------------------
struct PlayerState {
    PlayerStats  stats;
    PlayerStrategy strategy;      // resolved: player override merged with team default
    Zone zone{Zone::OwnHalf};
    bool prone{false};
    bool stunned{false};          // misses next turn
    bool ko{false};               // knocked out – may return next half
    bool casualty{false};         // out for the game
    bool hasBall{false};
    bool activated{false};          // already took an action this turn — prevents double-activation
    bool inReserves{false};         // waiting in reserves/bench (Riotous Rookies, etc.)
    bool isTeamCaptain{false};      // gains Pro; free team re-roll on natural 6 while on pitch
    bool distractedThisTurn{false};  // Bone Head or Really Stupid fired — no action, no tackle zone this turn
    int  stunTimer{0};

    bool isActive()   const { return !ko && !casualty && !inReserves; }
    // canAct: physically able AND not yet activated this turn
    bool canAct()     const { return isActive() && !stunned && !prone && !activated; }
    bool isOnPitch()  const { return !ko && !casualty && !inReserves; }
};

// ---------------------------------------------------------------------------
// In-game team state
// ---------------------------------------------------------------------------
struct TeamState {
    std::string name;
    std::string race;
    bool hasApothecary{false};
    bool apothecaryUsed{false};
    bool riotousRookies{false};
    PlayerState riotousRookieTemplate{};    // pre-built Snotling Lineman for Riotous Rookies
    std::array<PlayerState, 28> players{};  // 16 roster + 7 Riotous Rookies + 2 star players + 3 spare
    int playerCount{0};
    int rerollsRemaining{0};

    std::span<PlayerState>       allPlayers()       { return {players.data(), static_cast<size_t>(playerCount)}; }
    std::span<const PlayerState> allPlayers() const { return {players.data(), static_cast<size_t>(playerCount)}; }

    // Aggregate per-game stats
    int score{0};
    int blocksAttempted{0};
    int blocksSuccessful{0};
    int casualties{0};
    int knockouts{0};
    int touchdowns{0};
    int passesAttempted{0};
    int passesCompleted{0};

    bool captainActive() const {
        for (const auto& p : allPlayers())
            if (p.isTeamCaptain && p.isActive() && !p.stunned) return true;
        return false;
    }

    PlayerState* ballCarrier() {
        for (auto& p : allPlayers())
            if (p.hasBall) return &p;
        return nullptr;
    }

    int countUsable() const {
        int n = 0;
        for (const auto& p : allPlayers()) if (p.canAct())    ++n;
        return n;
    }

    int countOnPitch() const {
        int n = 0;
        for (const auto& p : allPlayers()) if (p.isOnPitch()) ++n;
        return n;
    }
};

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------
struct GameResult {
    int score1{}, score2{};
    int casualties1{}, casualties2{};
    int ko1{}, ko2{};
    int blocks1{}, blocks2{};
    int passes1{}, passes2{};
};

struct SimulationStats {
    int totalGames{};
    int wins1{}, wins2{}, draws{};
    long long totalScore1{}, totalScore2{};
    long long totalCasualties1{}, totalCasualties2{};
    long long totalKO1{}, totalKO2{};
    long long totalBlocks1{}, totalBlocks2{};
    long long totalPasses1{}, totalPasses2{};
};
