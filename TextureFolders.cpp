#include "TextureFolders.h"

#include "Config.h"

#include <atomic>
#include <exception>
#include <filesystem>
#include <map>
#include <mutex>
#include <stop_token>
#include <string_view>
#include <system_error>
#include <thread>

namespace {
    std::vector<TextureFolder> g_folders;
    std::atomic<bool>          g_ready{false};
    std::atomic<bool>          g_started{false};

    // Directories under textures in the base game's archives, which the disk
    // scan cannot see. Sorted, lower case, no leading or trailing separator.
    constexpr std::string_view kArchivedFolders = R"FOLDERS(
_byoh
_byoh\architecture
_byoh\architecture\byohhouse
_byoh\architecture\hearse
_byoh\clothes
_byoh\clothes\childrenclothesvariants
_byoh\clothes\childrenclothesvariants\f
_byoh\clothes\childrenclothesvariants\m
_byoh\clutter
_byoh\clutter\doll
_byoh\clutter\food
_byoh\clutter\house crafting
_byoh\clutter\ingredients
_byoh\clutter\resources
_byoh\clutter\wine
_byoh\furniture
_byoh\plants
_byoh\weapons
_byoh\weapons\woodensword
actors
actors\alduin
actors\bear
actors\bonewolf
actors\character
actors\character\argonianfemale
actors\character\argonianfemale\facedetails
actors\character\argonianmale
actors\character\beards
actors\character\bretonfemale
actors\character\bretonmale
actors\character\character assets
actors\character\character assets\tintmasks
actors\character\darkelffemale
actors\character\darkelfmale
actors\character\eyes
actors\character\facegendata
actors\character\facegendata\facetint
actors\character\facegendata\facetint\dawnguard.esm
actors\character\facegendata\facetint\dragonborn.esm
actors\character\facegendata\facetint\hearthfires.esm
actors\character\facegendata\facetint\skyrim.esm
actors\character\facegendata\facetint\update.esm
actors\character\facemods
actors\character\facemods\skyrim.esm
actors\character\female
actors\character\female\facedetails
actors\character\femalebrows
actors\character\femalechild
actors\character\femaleold
actors\character\femaleorc
actors\character\gore
actors\character\hair
actors\character\highelffemale
actors\character\highelfmale
actors\character\imperialfemale
actors\character\imperialmale
actors\character\khajiitfemale
actors\character\khajiitmale
actors\character\male
actors\character\male\facedetails
actors\character\malebrows
actors\character\malechild
actors\character\maleold
actors\character\manekin
actors\character\mouth
actors\character\orcmale
actors\character\redguardfemale
actors\character\redguardmale
actors\character\werewolf
actors\character\woodelffemale
actors\character\woodelfmale
actors\chaurus
actors\chicken
actors\dlc01
actors\dlc01\ancientfrostatronach
actors\dlc01\armoreddog
actors\dlc01\chaurusfrozen
actors\dlc01\dragon
actors\dlc01\elk
actors\dlc01\falmervampire
actors\dlc01\frostgiant
actors\dlc01\sabrecat
actors\dlc01\shadowmere
actors\dlc01\spriggan
actors\dlc01\vampirebrute
actors\dlc02
actors\dlc02\ashman
actors\dlc02\benthiclurker
actors\dlc02\boar
actors\dlc02\dlc2frostgiant
actors\dlc02\dragon
actors\dlc02\dragonpriestacolyte
actors\dlc02\dwarvenballistacenturion
actors\dlc02\hmdaedra
actors\dlc02\hulkingdraugr
actors\dlc02\mudcrabash
actors\dlc02\netch
actors\dlc02\riekling
actors\dlc02\scrib
actors\dlc02\spider_poison
actors\dlc02\sprigganburnt
actors\dlc02\stormatronachash
actors\dlc02\werebear
actors\dog
actors\dragon
actors\dragon priest
actors\draugr
actors\dwarvenspherecenturion
actors\dwarvenspidercenturion
actors\dwarvensteamcenturion
actors\elk
actors\falmer
actors\fireatronach
actors\fox
actors\frostatronach
actors\frostbitespider
actors\giant
actors\goat
actors\hagraven
actors\highlandcow
actors\horker
actors\horse
actors\icewraith
actors\mammoth
actors\mudcrab
actors\parthurnax
actors\rabbit
actors\sabrecat
actors\skeever
actors\skeleton
actors\slaughterfish
actors\spriggan
actors\stormatronach
actors\troll
actors\vampirelord
actors\wisp
actors\wolf
architecture
architecture\edgetrim
architecture\falmer hut
architecture\farmhouse
architecture\highhrothgar
architecture\markarth
architecture\riften
architecture\skyhaventemple
architecture\solitude
architecture\sovngarde
architecture\tents
architecture\whiterun
architecture\windhelm
architecture\winterhold
armor
armor\amuletsandrings
armor\amuletsandrings\akatosh
armor\amuletsandrings\ancientnord
armor\amuletsandrings\arkay
armor\amuletsandrings\dibella
armor\amuletsandrings\elder council
armor\amuletsandrings\julianos
armor\amuletsandrings\kynareth
armor\amuletsandrings\mara
armor\amuletsandrings\necromancer
armor\amuletsandrings\stendarr
armor\amuletsandrings\talismanoftreachery
armor\amuletsandrings\talos
armor\amuletsandrings\zenithar
armor\blades
armor\bonecrown
armor\briarheart
armor\circlets
armor\daedric
armor\dbarmor
armor\dragonbone
armor\dragonpriesthelm
armor\dragonscale
armor\draugr
armor\dwarven
armor\dwarven\f
armor\dwarven\m
armor\ebonyarmor
armor\ebonyarmor\f
armor\ebonyarmor\m
armor\ebonymail
armor\ebonymail\f
armor\ebonymail\m
armor\elven
armor\elven\f
armor\elven\m
armor\falmerarmor
armor\forsworn
armor\generaltulius
armor\glass
armor\glass\f
armor\glass\m
armor\glass\shield
armor\hide
armor\hide\f
armor\hide\m
armor\imperial
armor\imperial\f
armor\imperial\m
armor\iron
armor\iron\f
armor\iron\m
armor\nightingale
armor\nightingale\f
armor\nightingale\m
armor\nordplate
armor\orcish
armor\savior's hide
armor\shieldofysgramor
armor\sonsoftalos
armor\spellbreaker
armor\steel
armor\steel\m
armor\stormcloaks
armor\studded
armor\thievesguild
armor\thievesguild\f
armor\thievesguild\m
armor\tsun
armor\wolf
armor\yngolshelm
blood
clothes
clothes\archmage
clothes\archmage\f
clothes\archmage\m
clothes\bandages
clothes\bandit
clothes\barkeeper
clothes\barkeeper\f
clothes\barkeeper\m
clothes\beggarclothes
clothes\blacksmith
clothes\blacksmith\f
clothes\blacksmith\m
clothes\chef
clothes\chef\f
clothes\chef\m
clothes\childrenclothes
clothes\childrenclothes\f
clothes\childrenclothes\m
clothes\clavicusvilemask
clothes\emperor
clothes\emperor\f
clothes\emperor\m
clothes\executioner
clothes\executionhood
clothes\farmclothes01
clothes\farmclothes02
clothes\farmclothes02\f
clothes\farmclothes02\m
clothes\farmclothes02\variant
clothes\farmclothes03
clothes\farmclothes03\variant
clothes\farmclothes04
clothes\farmclothes04\variant
clothes\fineclothes01
clothes\fineclothes03
clothes\focusinggloves
clothes\gag
clothes\graybeardrobe
clothes\graybeardrobe\m
clothes\jester
clothes\mageapprentice
clothes\mageapprentice\f
clothes\mageapprentice\m
clothes\magejourneyman
clothes\magejourneyman\f
clothes\magejourneyman\m
clothes\merchantclothes
clothes\merchantclothes\variant
clothes\minerclothes
clothes\minerclothes\variant
clothes\monk
clothes\mournersclothes
clothes\necromancer
clothes\nocturnal
clothes\prisoner
clothes\prisoner\f
clothes\prisoner\m
clothes\psiijic
clothes\redguard
clothes\robedarkbrotherhood
clothes\robemythicdawn
clothes\sheogorath
clothes\thalmor
clothes\warlock
clothes\weddingdress
clothes\wench
clothes\wounds
clothes\yarl
clothes\yarl\f
clothes\yarl\m
clothes\yarlclothes02
clothes\yarlclothes03
clutter
clutter\barset
clutter\blackpool
clutter\blacksmith
clutter\bloodyrags
clutter\bones
clutter\books
clutter\burntcorpses
clutter\candles
clutter\carts
clutter\caves
clutter\charcoal
clutter\chauruseggs
clutter\choppingblock
clutter\common
clutter\containers
clutter\cowhide
clutter\daedric
clutter\dawnbreakerpedestal
clutter\deadanimals
clutter\diningset
clutter\dragon
clutter\dragonmap
clutter\dwemer
clutter\elderscroll
clutter\falmerrosettastone
clutter\falmertotems
clutter\firewood
clutter\food
clutter\goatskin
clutter\grayfoxbust
clutter\hay
clutter\horkertusk
clutter\imperial
clutter\ingredients
clutter\ingredients\clam
clutter\kingolafeffigy
clutter\kitchen
clutter\ladder
clutter\lumbermill
clutter\meadery
clutter\nightmother
clutter\pelagiushipbone
clutter\quest
clutter\ruins
clutter\sabrecatpelts
clutter\shadowmarks
clutter\shrines
clutter\signage
clutter\signage\inns
clutter\signage\riften
clutter\signage\riverwood
clutter\signage\roadsigns
clutter\signage\solitude
clutter\skeletonkey
clutter\skullpedestal
clutter\smelter
clutter\snail
clutter\soulgem
clutter\spear
clutter\statues
clutter\stockade
clutter\warhorns
clutter\werewolf
clutter\werewolfskull
clutter\werewolftotems
clutter\wine
clutter\wolfpelts
clutter\woodfires
creationclub
creationclub\_shared
creationclub\_shared\clutter
creationclub\_shared\clutter\ingredients
creationclub\_shared\dungeons
creationclub\_shared\dungeons\ayleidruins
creationclub\_shared\dungeons\root
creationclub\_shared\dungeons\root\clutter
creationclub\_shared\landscape
creationclub\_shared\landscape\trees
creationclub\_shared\landscape\trees\mania
creationclub\_shared\plants
creationclub\_shared\plants\rootdungeon
critters
critters\fish
cubemaps
dlc01
dlc01\actors
dlc01\actors\armoredtroll
dlc01\actors\chaurusflyer
dlc01\actors\falmer
dlc01\actors\forgemaster
dlc01\actors\undeaddragon
dlc01\actors\vampiredog
dlc01\actors\vampirelord
dlc01\architecture
dlc01\architecture\byohhouse
dlc01\architecture\dawnguard
dlc01\architecture\falmerbridge
dlc01\architecture\falmerhut
dlc01\architecture\hearse
dlc01\architecture\snowelfruins
dlc01\armor
dlc01\armor\amuletsandrings
dlc01\armor\auriel
dlc01\armor\circlets
dlc01\armor\dawnguard
dlc01\armor\dwarven
dlc01\armor\falmer
dlc01\armor\ivoryarmor
dlc01\clothes
dlc01\clothes\blindmothpriest
dlc01\clothes\prisoner
dlc01\clothes\prisoner\m
dlc01\clothes\vampire
dlc01\clothes\vampsimpleclothes
dlc01\clutter
dlc01\clutter\aurielstatue
dlc01\clutter\books
dlc01\clutter\chauruscocoon
dlc01\clutter\house crafting
dlc01\clutter\ingredients
dlc01\clutter\meadery
dlc01\clutter\quest
dlc01\clutter\vampireremains
dlc01\clutter\wine
dlc01\critters
dlc01\cubemaps
dlc01\dungeons
dlc01\dungeons\castle
dlc01\dungeons\caves
dlc01\effects
dlc01\effects\gradients
dlc01\furniture
dlc01\interface
dlc01\landscape
dlc01\landscape\grass
dlc01\landscape\plants
dlc01\landscape\trees
dlc01\lod
dlc01\plants
dlc01\plants\caveworm
dlc01\sky
dlc01\soulcairn
dlc01\weapons
dlc01\weapons\auriel
dlc01\weapons\crossbow
dlc01\weapons\dragonbone
dlc01\weapons\dwarven
dlc01\weapons\dwarvencrossbow
dlc01\weapons\falmerstaff
dlc01\weapons\hunter axe
dlc01\weapons\hunter hammer
dlc01\weapons\vampire
dlc02
dlc02\architecture
dlc02\architecture\edgetrim
dlc02\architecture\ravenrock
dlc02\architecture\ravenrock\bulwark
dlc02\architecture\ravenrock\redoran
dlc02\architecture\telvannitower
dlc02\architecture\thirsk
dlc02\architecture\thirsk\meadhall
dlc02\armor
dlc02\armor\acolytemasks
dlc02\armor\amulets
dlc02\armor\amulets\eastempirecompany
dlc02\armor\amulets\skaal
dlc02\armor\bonemold
dlc02\armor\chitinheavy
dlc02\armor\chitinlight
dlc02\armor\chitinshield
dlc02\armor\generalcarius
dlc02\armor\nordiccarved
dlc02\armor\nordicshield
dlc02\armor\stahlrimheavy
dlc02\armor\stahlrimlight
dlc02\armor\stalhrimshield
dlc02\clothes
dlc02\clothes\cultist
dlc02\clothes\darkelf
dlc02\clothes\miraakrobes
dlc02\clothes\skaal
dlc02\clothes\telvanni
dlc02\clothes\templeoutfit
dlc02\clutter
dlc02\clutter\books
dlc02\clutter\food
dlc02\clutter\heartstone
dlc02\clutter\ingredients
dlc02\clutter\misc
dlc02\clutter\pearloyster
dlc02\critters
dlc02\cubemaps
dlc02\dungeons
dlc02\dungeons\apocrypha
dlc02\dungeons\dwemer
dlc02\effects
dlc02\effects\gradients
dlc02\furniture
dlc02\landscape
dlc02\landscape\grass
dlc02\landscape\standingstone
dlc02\landscape\trees
dlc02\lod
dlc02\plants
dlc02\prototype
dlc02\sky
dlc02\weapons
dlc02\weapons\ashman
dlc02\weapons\bloodsword
dlc02\weapons\dwarven
dlc02\weapons\hammer
dlc02\weapons\hrothmundsaxe
dlc02\weapons\miraak
dlc02\weapons\nordic
dlc02\weapons\nordic\axe_small
dlc02\weapons\nordic\axe_tall
dlc02\weapons\nordic\bow_nordic
dlc02\weapons\nordic\dagger_nordic
dlc02\weapons\nordic\hammer_nordic
dlc02\weapons\nordic\mace_nordic
dlc02\weapons\nordic\quiver
dlc02\weapons\nordic\sword_nordic
dlc02\weapons\nordic\sword_tall
dlc02\weapons\rieklingspears
dlc02\weapons\stahlrim
dungeons
dungeons\azurasstar
dungeons\caves
dungeons\dwemerruins
dungeons\imperial
dungeons\mines
dungeons\nordic
dungeons\riften
dungeons\ships
effects
effects\cockroach
effects\gradients
effects\lensflares
effects\trailerfx
furniture
furniture\bedroll
furniture\noble
furniture\prisonercarriage
impactdecals
interface
interface\books
interface\books\alchemy ingredients
interface\books\arcane scribblings
interface\books\constellation
interface\books\daedric artifact book
interface\books\illuminated_letters
interface\books\map_illustration
interface\books\shadowmarks
interface\objects
interface\objects\lockpicking
landscape
landscape\dirtcliffs
landscape\grass
landscape\mountains
landscape\roads
landscape\trees
lod
plants
puddle
shadertests
sky
terrain
terrain\blackreach
terrain\blackreach\objects
terrain\deepwoodredoubtworld
terrain\deepwoodredoubtworld\objects
terrain\deepwoodredoubtworld\trees
terrain\dlc01falmervalley
terrain\dlc01falmervalley\objects
terrain\dlc01falmervalley\trees
terrain\dlc01soulcairn
terrain\dlc01soulcairn\objects
terrain\dlc1hunterhqworld
terrain\dlc1hunterhqworld\objects
terrain\dlc1hunterhqworld\trees
terrain\dlc2apocryphaworld
terrain\dlc2apocryphaworld\objects
terrain\dlc2solstheimworld
terrain\dlc2solstheimworld\objects
terrain\dlc2solstheimworld\trees
terrain\japhetsfollyworld
terrain\japhetsfollyworld\objects
terrain\markarthworld
terrain\markarthworld\objects
terrain\skuldafnworld
terrain\skuldafnworld\objects
terrain\skuldafnworld\trees
terrain\sovngarde
terrain\sovngarde\objects
terrain\sovngarde\trees
terrain\tamriel
terrain\tamriel\objects
terrain\tamriel\trees
test
trap
water
water\skyrim.esm
weapons
weapons\akaviri
weapons\axeofysgramor
weapons\bladeofwoe
weapons\blades
weapons\ceremonialblade
weapons\daedric
weapons\dawnbreaker
weapons\dragon priest
weapons\draugr
weapons\dwarven
weapons\ebony
weapons\ebonyblade
weapons\elven
weapons\elven\arrow
weapons\elven\battleaxe
weapons\elven\bow
weapons\elven\dagger
weapons\elven\greatsword
weapons\elven\handaxe
weapons\elven\mace
weapons\elven\scabbard
weapons\elven\shield
weapons\elven\sword
weapons\elven\warhammer
weapons\executioneraxe
weapons\falmer
weapons\forsworn
weapons\giant
weapons\glass
weapons\imperial
weapons\iron
weapons\keening
weapons\maceofmolagbal
weapons\mehrunesrazor
weapons\nettlebane
weapons\nightingale
weapons\nordhero
weapons\orcish
weapons\rueful axe
weapons\sanguinerose
weapons\scimitar
weapons\shiv
weapons\silver
weapons\skullofcorruption
weapons\spikedshield
weapons\staff01
weapons\staff02
weapons\staff03
weapons\staff04
weapons\staffofmagnus
weapons\steel
weapons\torch
weapons\troll
weapons\volendrung
weapons\wabbajack
weapons\wooden
)FOLDERS";

    // The literal above starts with a newline, so the first split is empty.
    void AddArchivedFolders(std::map<std::string, std::uint32_t>& counts) {
        std::string_view remaining = kArchivedFolders;

        while (!remaining.empty()) {
            const auto end  = remaining.find('\n');
            const auto line = remaining.substr(0, end);

            if (!line.empty()) counts.emplace(line, 0);
            if (end == std::string_view::npos) return;

            remaining.remove_prefix(end + 1);
        }
    }

    // Not string(): that one converts to the system code page and throws on any
    // character it can't map, which a mod folder can easily contain. Fold only
    // touches ASCII, so the rest of a UTF-8 sequence passes through untouched.
    std::string FoldedPath(const std::filesystem::path& path) {
        const auto native = path.u8string();

        std::string text(reinterpret_cast<const char*>(native.data()), native.size());
        for (auto& c : text) c = Fold(c);
        return text;
    }

    bool IsDds(const std::filesystem::path& path) {
        const auto extension = FoldedPath(path.extension());
        return extension == ".dds";
    }

    void ScanFolders(std::stop_token stop) {
        std::map<std::string, std::uint32_t> counts;
        AddArchivedFolders(counts);

        const std::filesystem::path root{"Data/textures"};

        std::error_code ec;
        std::filesystem::recursive_directory_iterator it{
            root, std::filesystem::directory_options::skip_permission_denied, ec};

        const std::filesystem::recursive_directory_iterator end;

        for (; !ec && it != end; it.increment(ec)) {
            if (stop.stop_requested()) return;

            const auto& entry = *it;

            if (entry.is_directory(ec)) {
                counts.emplace(FoldedPath(entry.path().lexically_relative(root)), 0);
            } else if (entry.is_regular_file(ec) && IsDds(entry.path())) {
                const auto parent = entry.path().parent_path().lexically_relative(root);
                ++counts[FoldedPath(parent)];
            }

            ec.clear();
        }

        std::vector<TextureFolder> folders;
        folders.reserve(counts.size());

        for (const auto& [path, files] : counts)
            if (!path.empty()) folders.emplace_back(path, files);

        g_folders = std::move(folders);
        g_ready.store(true, std::memory_order_release);

        SKSE::log::info("Folder list ready, {} directories", g_folders.size());
    }

    // Anything escaping here would take the process down without a crash log,
    // since this runs on a thread of its own.
    void Scan(std::stop_token stop) {
        try {
            ScanFolders(std::move(stop));
        } catch (const std::exception& e) {
            SKSE::log::warn("Folder scan stopped: {}", e.what());
        } catch (...) {
            SKSE::log::warn("Folder scan stopped");
        }
    }

    // Joined when the DLL unloads. The stop token keeps that from waiting on a
    // scan still walking a large install.
    std::jthread g_scanner;

    std::mutex                                     g_usedMutex;
    std::unordered_map<std::string, std::uint32_t> g_used;

    // textures\armor\daedric\cuirass.dds becomes armor\daedric, which is how
    // the scan names the same folder. Empty when the file sits at the root or
    // the name doesn't look like a path.
    bool FolderFromName(std::string_view name, std::string& folder) {
        folder.clear();
        for (const auto c : name) folder.push_back(Fold(c));

        constexpr std::string_view kRoot = "textures\\";

        const auto root = folder.find(kRoot);
        if (root != std::string::npos) folder.erase(0, root + kRoot.size());

        const auto slash = folder.find_last_of('\\');
        if (slash == std::string::npos) return false;

        folder.resize(slash);
        return !folder.empty();
    }
}

void BeginFolderScan() {
    if (g_started.exchange(true)) return;

    g_scanner = std::jthread(Scan);
}

bool FolderScanReady() { return g_ready.load(std::memory_order_acquire); }

const std::vector<TextureFolder>& ScannedFolders() { return g_folders; }

std::string FolderRuleText(std::string_view path) {
    std::string rule;
    rule.reserve(path.size() + 2);

    rule.push_back('\\');
    rule.append(path);
    rule.push_back('\\');

    return rule;
}

void RecordUsedFolder(std::string_view name) {
    // Reused per thread, so a folder the table already knows costs no
    // allocation at all.
    thread_local std::string folder;

    if (!FolderFromName(name, folder)) return;

    const std::scoped_lock lock(g_usedMutex);

    if (const auto it = g_used.find(folder); it != g_used.end()) {
        ++it->second;
        return;
    }

    g_used.emplace(folder, 1);
}

std::unordered_map<std::string, std::uint32_t> UsedFolders() {
    const std::scoped_lock lock(g_usedMutex);
    return g_used;
}

void ClearUsedFolders() {
    const std::scoped_lock lock(g_usedMutex);
    g_used.clear();
}
