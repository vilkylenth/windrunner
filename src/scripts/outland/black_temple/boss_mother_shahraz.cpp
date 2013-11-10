#include "precompiled.h"
#include "def_black_temple.h"

//Speech'n'Sounds
enum Says
{
    SAY_TAUNT1              = -1564018,
    SAY_TAUNT2              = -1564019,
    SAY_TAUNT3              = -1564020,
    SAY_AGGRO               = -1564021,
    SAY_SPELL1              = -1564022,
    SAY_SPELL2              = -1564023,
    SAY_SPELL3              = -1564024,
    SAY_SLAY1               = -1564025,
    SAY_SLAY2               = -1564026,
    SAY_ENRAGE              = -1564027,
    SAY_DEATH               = -1564028
};

//Spells
enum Spells
{
    SPELL_BEAM_SINISTER             = 40859,
    SPELL_BEAM_SINISTER_TRIGGER     = 40863,
    SPELL_BEAM_VILE                 = 40860,
    SPELL_BEAM_VILE_TRIGGER         = 40865,
    SPELL_BEAM_WICKED               = 40861,
    SPELL_BEAM_WICKED_TRIGGER       = 40866,
    SPELL_BEAM_SINFUL               = 40827,
    SPELL_BEAM_SINFUL_TRIGGER       = 40862,
    SPELL_ATTRACTION_DUMMY          = 40870, //unused
    SPELL_ATTRACTION                = 40871, //AoE damage + dummy aura on ally (modified to duration 2.0 instead of 1.1)
    SPELL_ATTRACTION_VIS            = 41001, //only visual, periodically trigger 40870
    SPELL_SILENCING_SHRIEK          = 40823,
    SPELL_ENRAGE                    = 23537,
    SPELL_SABER_LASH                = 40810, //76k damage (43267 = same with 47500 damage )
    SPELL_SABER_LASH_TRIGGER        = 40816, //trigger proc 40810 & periodic 43690
    SPELL_SABER_LASH_IMM            = 43690, //immunity
    SPELL_TELEPORT_VISUAL           = 40869,
    SPELL_BERSERK                   = 45078,
    
    SPELL_PRISMATIC_SHIELD          = 40879 //cast random prismatic auras
};

enum Timers 
{
    TIMER_BEAM_FIRST = 12000,
    TIMER_BEAM = 9000,
    TIMER_PRISMATIC_SHIELD = 15000,
    TIMER_FATAL_ATTRACTION_FIRST = 12000,
    TIMER_SILENCING_SHRIEK = 30000,
    TIMER_SABER_LASH_FIRST = 11000,
    TIMER_SABER_LASH = 35000,
    TIMER_ENRAGE = 600000
};

#define TIMER_RANDOM_YELL 70000 + rand()%41 * 1000
#define TIMER_FATAL_ATTRACTION 18000 + rand()%7000
/*
uint32 PrismaticAuras[]=
{
    40880,                                                  // Shadow
    40882,                                                  // Fire
    40883,                                                  // Nature
    40891,                                                  // Arcane
    40896,                                                  // Frost
    40897,                                                  // Holy
};
*/
struct Locations
{
    float x,y,z;
};

static Locations TeleportPoint[]=
{
    /*{959.996, 212.576, 193.843},
    {932.537, 231.813, 193.838},
    {958.675, 254.767, 193.822},
    {946.955, 201.316, 192.535},
    {944.294, 149.676, 197.551},
    {930.548, 284.888, 193.367},
    {965.997, 278.398, 195.777}*/
    {959.996, 212.576, 195.215},
    {932.537, 231.813, 195.215},
    {958.675, 254.767, 195.215},
    {946.955, 201.316, 192.535},
    {944.294, 149.676, 200.175},
    {930.548, 284.888, 195.215},
    {965.997, 278.398, 198.215}
};

struct boss_shahrazAI : public ScriptedAI
{
    boss_shahrazAI(Creature *c) : ScriptedAI(c)
    {
        pInstance = ((ScriptedInstance*)c->GetInstanceData());
    }

    ScriptedInstance* pInstance;

    uint64 AttractionTargetGUID[3];
    uint32 BeamTimer;
    uint32 BeamCount;
    uint32 CurrentBeam;
    uint32 FatalAttractionTimer;
    uint32 FatalAttractionExplodeTimer;
    uint32 ShriekTimer;
    uint32 SaberTimer;
    uint32 RandomYellTimer;
    uint32 EnrageTimer;
    uint32 CheckPlayersUndermapTimer;
    uint32 TooFarAwayCheckTimer;
    bool checkFatalAttractionDistance;

    bool Enraged;

    void Reset()
    {
        if(pInstance && m_creature->isAlive())
            pInstance->SetData(DATA_MOTHERSHAHRAZEVENT, NOT_STARTED);

        memset(AttractionTargetGUID, 0, sizeof(uint64) * 3);

        BeamTimer = TIMER_BEAM;
        CurrentBeam = rand()%4;                                    // 0 - Sinister, 1 - Vile, 2 - Wicked, 3 - Sinful
        BeamCount = 0;
        FatalAttractionTimer = TIMER_FATAL_ATTRACTION_FIRST;
        FatalAttractionExplodeTimer = -1;
        ShriekTimer = TIMER_SILENCING_SHRIEK;
        SaberTimer = TIMER_SABER_LASH_FIRST;
        RandomYellTimer = TIMER_RANDOM_YELL;
        EnrageTimer = TIMER_ENRAGE;
        CheckPlayersUndermapTimer = -1;
        TooFarAwayCheckTimer = 2000;
        checkFatalAttractionDistance = false;

        Enraged = false;
    }

    void Aggro(Unit *who)
    {
        if(pInstance)
            pInstance->SetData(DATA_MOTHERSHAHRAZEVENT, IN_PROGRESS);

        DoZoneInCombat();
        DoScriptText(SAY_AGGRO, me);
        DoCast(me,SPELL_PRISMATIC_SHIELD,true);
    }

    void KilledUnit(Unit *victim)
    {
        DoScriptText(RAND(SAY_SLAY1,SAY_SLAY2), m_creature);
    }

    void JustDied(Unit *victim)
    {
        if(pInstance)
            pInstance->SetData(DATA_MOTHERSHAHRAZEVENT, DONE);

        DoScriptText(SAY_DEATH, m_creature);
    }

    bool TeleportPlayers()
    {
        uint32 random = rand()%7;
        float X = TeleportPoint[random].x;
        float Y = TeleportPoint[random].y;
        float Z = TeleportPoint[random].z;
        uint8 teleportedCount = 0;
        std::list<Unit*> targetList;
        SelectUnitList(targetList, 3, SELECT_TARGET_RANDOM, 120.0f, true, SPELL_SABER_LASH_IMM, 0);
        if(targetList.size() == 3)
        {
            uint8 i = 0;
            for(auto target : targetList)
            {
                AttractionTargetGUID[i] = target->GetGUID();
                target->CastSpell(target, SPELL_TELEPORT_VISUAL, true);
                DoCast(target,SPELL_ATTRACTION_VIS,true);
                DoTeleportPlayer(target, X, Y, Z, target->GetOrientation());
                i++;
            }
            return true;
        }
        return false;
    }

    void UpdateAI(const uint32 diff)
    {
        if(!UpdateVictim())
            return;
            
        if (TooFarAwayCheckTimer <= diff) {
            if (m_creature->GetPositionZ() < 180.0f) {
                EnterEvadeMode();
                return;
            }
                
            TooFarAwayCheckTimer = 2000;
        }
        else
            TooFarAwayCheckTimer -= diff;

        // Only check the last 3 teleported players
        if (CheckPlayersUndermapTimer < diff) {
            for (int i = 0; i < 3; i++) {
                if (Player* plr = Unit::GetPlayer(AttractionTargetGUID[i])) {
                    float z = plr->GetPositionZ();
                    if (z < 189)      // Player seems to be undermap (ugly hack, isn't it ?)
                        DoTeleportPlayer(plr, 945.6173, 198.3479, 192.00, 4.674);
                }
            }
            
            CheckPlayersUndermapTimer = -1;
        }else CheckPlayersUndermapTimer -= diff;

        // Cast beam and randomize it every 4 beams
        if(BeamTimer < diff)
        {
            Unit* target = SelectUnit(SELECT_TARGET_RANDOM, 15.0f, 80.0f, true); //prevent casting it on tanks
            if(!target)
                target = SelectUnit(SELECT_TARGET_RANDOM, 0.0f, 80.0f, true);

            if(!target)
                return;

            BeamTimer = 9000;

            switch(CurrentBeam)
            {
                case 0:
                    DoCast(target, SPELL_BEAM_SINISTER);
                    break;
                case 1:
                    DoCast(target, SPELL_BEAM_VILE);
                    break;
                case 2:
                    DoCast(target, SPELL_BEAM_WICKED);
                    break;
                case 3:
                    DoCast(target, SPELL_BEAM_SINFUL);
                    break;
            }
            BeamCount++;
            uint32 Beam = CurrentBeam;
            if(BeamCount > 3)
            {
                while(CurrentBeam == Beam) //pick a different one
                    CurrentBeam = rand()%4;
                BeamCount = 0;
            }

        }else BeamTimer -= diff;

        // Select 3 random targets, teleport to a random location then make them cast explosions until they get away from each other.
        if(FatalAttractionTimer < diff)
        {
            if(TeleportPlayers())
            {
                DoScriptText(RAND(SAY_SPELL2,SAY_SPELL3), m_creature);
                FatalAttractionExplodeTimer = 2500;
                checkFatalAttractionDistance = false;
                CheckPlayersUndermapTimer = 2500;
            }
            FatalAttractionTimer = TIMER_FATAL_ATTRACTION;
        }else FatalAttractionTimer -= diff;
        
        //Get targets & cast aoe
        if(FatalAttractionExplodeTimer < diff)
        {
            bool clear = true;
            for(uint8 i = 0; i < 3; ++i)
            {
                if(!AttractionTargetGUID[i]) continue;

                Player* p = Player::GetPlayer(AttractionTargetGUID[i]);
                if(p && p->isAlive())
                {
                    //Clear fatal attraction target (only after first cast)
                    if(checkFatalAttractionDistance)
                    {  
                        if(!p->HasAuraWithCasterNot(SPELL_ATTRACTION,1,p->GetGUID())) //dummy aura applied on close allies on every aoe for 2.0
                        {
                            p->RemoveAurasDueToSpell(SPELL_ATTRACTION_VIS);
                            AttractionTargetGUID[i] = 0;
                            continue;
                        }
                    }

                    // Else cast SPELL_ATTRACTION
                    p->CastSpell((Unit*)NULL,SPELL_ATTRACTION,true);
                    clear = false;
                }
            }

            checkFatalAttractionDistance = true;
            if(clear)
                FatalAttractionExplodeTimer = -1;
            else
                FatalAttractionExplodeTimer = 1000;

        } else FatalAttractionExplodeTimer -= diff;
        
        if(ShriekTimer < diff)
        {
            if(DoCast(m_creature->getVictim(), SPELL_SILENCING_SHRIEK))
                ShriekTimer = TIMER_SILENCING_SHRIEK;
        } else ShriekTimer -= diff;

        if(SaberTimer < diff)
        {
            DoCast(m_creature->getVictim(), SPELL_SABER_LASH);
            SaberTimer = TIMER_SABER_LASH;
        } else SaberTimer -= diff;

        //Enrage
        if(!m_creature->HasAura(SPELL_BERSERK, 0))
            if(EnrageTimer < diff)
        {
            DoCast(m_creature, SPELL_BERSERK);
            DoScriptText(SAY_ENRAGE, m_creature);
        } else EnrageTimer -= diff;

        //Random taunts
        if(RandomYellTimer < diff)
        {
            DoScriptText(RAND(SAY_TAUNT1,SAY_TAUNT2,SAY_TAUNT3), m_creature);
            RandomYellTimer = TIMER_RANDOM_YELL;
        }else RandomYellTimer -= diff;

        DoMeleeAttackIfReady();
    }
};

CreatureAI* GetAI_boss_shahraz(Creature *_Creature)
{
    return new boss_shahrazAI (_Creature);
}

void AddSC_boss_mother_shahraz()
{
    Script *newscript;
    newscript = new Script;
    newscript->Name="boss_mother_shahraz";
    newscript->GetAI = &GetAI_boss_shahraz;
    newscript->RegisterSelf();
}

