/*
Rodent, a UCI chess playing engine derived from Sungorus 1.4
Copyright (C) 2009-2011 Pablo Vazquez (Sungorus author)
Copyright (C) 2011-2019 Pawel Koziol

Rodent is free software: you can redistribute it and/or modify it under the terms of the GNU
General Public License as published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

Rodent is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License along with this program.
If not, see <http://www.gnu.org/licenses/>.
*/

#include "rodent.h"
#include "eval.h"
#include <cstdio>
#include <cstring>
#include <cmath> // round

void cEngine::ClearAll() {

    ClearPawnHash();
    ClearEvalHash();
    ClearHist();
}

void cEngine::ClearEvalHash() {

    ZEROARRAY(mEvalTT);
}

void cEngine::EvaluateMaterial(POS *p, eData *e, eColor sd) {

    eColor op = ~sd;

    int tmp = Par.np_table[p->mCnt[sd][P]] * p->mCnt[sd][N]    // knights lose value as pawns disappear
            - Par.rp_table[p->mCnt[sd][P]] * p->mCnt[sd][R];   // rooks gain value as pawns disappear

    if (p->mCnt[sd][N] > 1) tmp += V(N_PAIR);         // knight pair
    if (p->mCnt[sd][R] > 1) tmp += V(R_PAIR);         // rook pair
    if (p->mCnt[sd][B] > 1) tmp += V(B_PAIR);         // bishop pair

    // "elephantiasis correction" for queen, idea by H.G.Mueller (nb. rookVsQueen doesn't help)

    if (p->mCnt[sd][Q])
        tmp -= V(ELEPH) * (p->mCnt[op][N] + p->mCnt[op][B]);

    Add(e, sd, tmp);
}

void cEngine::EvaluatePieces(POS *p, eData *e, eColor sd) {

    U64 pieces, attack, control, possibleOutpost, contact, kingZone, file;
    int sq, cnt, ownPawnCount, oppPawnCount;
    int r_on_7th = 0;
    int mob_mg = 0;
    int mob_eg = 0;
    int tropism = 0;
    int lines_mg = 0;
    int lines_eg = 0;
    int fwd_weight = 0;
    int fwd_cnt = 0;
    int outpost_mg = 0;
	int outpost_eg = 0;
    int center_control = 2 * PopCnt(e->p_takes[sd] & Mask.center);

    // Init king attack zone

    eColor op = ~sd;
    int king_sq = p->KingSq(op);
    kingZone = BB.KingAttacks(king_sq);
    kingZone |= BB.ShiftFwd(kingZone, op);

	// Factor in minor pieces as king defenders

	int defenders[5] = { -5, 0, 3, 6, 9 };
	U64 minors = p->Knights(~sd) | p->Bishops(~sd);
	cnt = Min(PopCnt(minors & kingZone), 4);
	Add(e, ~sd, defenders[cnt], 0);

    // Init helper bitboards

    U64 n_checks = BB.KnightAttacks(king_sq) & ~p->mClBb[sd] & ~e->p_takes[op];
    U64 b_checks = BB.BishAttacks(p->OccBb(), king_sq) & ~p->mClBb[sd] & ~e->p_takes[op];
    U64 r_checks = BB.RookAttacks(p->OccBb(), king_sq) & ~p->mClBb[sd] & ~e->p_takes[op];
    U64 q_checks = r_checks & b_checks;
    U64 bb_excluded = p->Pawns(sd) | e->p_takes[op];

    // Knight eval

    pieces = p->Knights(sd);
    while (pieces) {
        sq = PopFirstBit(&pieces);                           // get square

        // knight tropism to enemy king (based on Hakapeliitta)

        tropism += Dist.knightTropism[sq][king_sq];

        if (SqBb(sq) & Mask.away[sd]) {                          // forwardness (based on Toga II 3.0)
            fwd_weight += V(N_FWD);
            fwd_cnt += 1;
        }

        control = BB.KnightAttacks(sq) & ~p->mClBb[sd];       // get control bitboard
        center_control += PopCnt(control & Mask.center);
        if (!(control  & ~e->p_takes[op] & Mask.away[sd]))    // we do not attack enemy half of the board
            Add(e, sd, V(N_OWH_MG), V(N_OWH_EG) );
        e->all_att[sd] |= BB.KnightAttacks(sq);
        e->ev_att[sd]  |= control;
        if (control & n_checks) e->att[sd] += V(N_CHK);// check threats

        possibleOutpost = control & ~e->p_takes[op];              // reachable outposts
        possibleOutpost &= ~e->p_can_take[op];
        possibleOutpost &= Mask.outpost_map[sd];
        if (possibleOutpost) Add(e, sd, V(N_REACH_MG), V(N_REACH_EG));

        attack = BB.KnightAttacks(sd);
        if (attack & kingZone) {                               // king attack
            e->wood[sd]++;
            e->att[sd] += V(N_ATT1) * PopCnt(attack & (kingZone & ~e->p_takes[op]));
            e->att[sd] += V(N_ATT2) * PopCnt(attack & (kingZone & e->p_takes[op]));
        }

        cnt = PopCnt(control & ~e->p_takes[op]);           // get mobility count
        mob_mg += Par.n_mob_mg[cnt];
        mob_eg += Par.n_mob_eg[cnt];

		EvaluateShielded(p, e, sd, sq, V(N_SH_MG), V(N_SH_EG), &outpost_mg, &outpost_eg);   // knight shielded by a pawn
        EvaluateOutpost(p, e, sd, N, sq, &outpost_mg, &outpost_eg);    // outpost
    }

    // Bishop eval

    pieces = p->Bishops(sd);
    while (pieces) {
        sq = PopFirstBit(&pieces);                         // get square

        // bishop tropism  to enemy king (based on Hakapeliitta)

        tropism += Dist.bishopTropism[sq][king_sq];

        if (SqBb(sq) & Mask.away[sd]) {                          // forwardness (based on Toga II 3.0)
            fwd_weight += V(B_FWD);
            fwd_cnt += 1;
        }

        control = BB.BishAttacks(p->OccBb(), sq);             // get control bitboard
        center_control += PopCnt(control & Mask.center);
        e->all_att[sd] |= control;                            // update attack map
        e->ev_att[sd]  |= control;
        if (!(control & Mask.away[sd]))
             Add(e, sd, V(B_OWH_MG), V(B_OWH_EG) ); // we do not attack enemy half of the board
		if (control & b_checks) {
			e->att[sd] += V(B_CHK);  // check threats
		}

        attack = BB.BishAttacks(p->OccBb() ^ p->Queens(sd), sq);  // get king attack bitboard

        if (attack & kingZone) {                               // evaluate king attacks
            e->wood[sd]++;
            e->att[sd] += V(B_ATT1) * PopCnt(attack & (kingZone & ~e->p_takes[op]));
            e->att[sd] += V(B_ATT2) * PopCnt(attack & (kingZone & e->p_takes[op]));
        }

        cnt = PopCnt(control &~bb_excluded); // get mobility count
        mob_mg += Par.b_mob_mg[cnt];
        mob_eg += Par.b_mob_eg[cnt];

        possibleOutpost = control & ~e->p_takes[op];              // reachable outposts
        possibleOutpost &= ~e->p_can_take[op];
        possibleOutpost &= Mask.outpost_map[sd];
        if (possibleOutpost) Add(e, sd, V(B_REACH_MG), V(B_REACH_EG));

		EvaluateShielded(p, e, sd, sq, V(B_SH_MG), V(B_SH_EG), &outpost_mg, &outpost_eg);  // bishop shielded by a pawn
        EvaluateOutpost(p, e, sd, B, sq, &outpost_mg, &outpost_eg);              // outpost

        // Pawns on the same square color as our bishop

        if (bbWhiteSq & SqBb(sq)) {
            ownPawnCount = PopCnt(bbWhiteSq & p->Pawns(sd)) - 4;
            oppPawnCount = PopCnt(bbWhiteSq & p->Pawns(op)) - 4;
        } else {
            ownPawnCount = PopCnt(bbBlackSq & p->Pawns(sd)) - 4;
            oppPawnCount = PopCnt(bbBlackSq & p->Pawns(op)) - 4;
        }

        // TODO: perhaps slightly bigger endgame penalties
        Add(e, sd, V(B_OWN_P) * ownPawnCount
                 + V(B_OPP_P) * oppPawnCount);
    }

    // Rook eval

    pieces = p->Rooks(sd);
    while (pieces) {
        sq = PopFirstBit(&pieces);                         // get square

        // rook tropism to enemy king (based on Hakapeliitta)

        tropism += Dist.rookTropism[sq][king_sq];

        if (SqBb(sq) & Mask.away[sd]) {                          // forwardness (based on Toga II 3.0)
            fwd_weight += V(R_FWD);
            fwd_cnt += 1;
        }

        control = BB.RookAttacks(p->OccBb(), sq);             // get control bitboard
        e->all_att[sd] |= control;                            // update attack map
        e->ev_att[sd] |= control;

        if ((control & ~p->mClBb[sd] & r_checks)
        && p->Queens(sd)) {
            e->att[sd] += V(R_CHK);                              // check threat bonus
            contact = (control & BB.KingAttacks(king_sq)) & r_checks;  // get contact check bitboard

            while (contact) {
                int contactSq = PopFirstBit(&contact);     // find a potential contact check
                if (p->Swap(sq, contactSq) >= 0) {               // rook exchanges are also accepted
                    e->att[sd] += V(R_CONTACT);
                    break;
                }
            }
        }

        attack = BB.RookAttacks(p->OccBb() ^ p->StraightMovers(sd), sq);// get king attack bitboard

        if (attack & kingZone) {                                         // evaluate king attacks
            e->wood[sd]++;
            e->att[sd] += V(R_ATT1) * PopCnt(attack & (kingZone & ~e->p_takes[op]));
            e->att[sd] += V(R_ATT2) * PopCnt(attack & (kingZone & e->p_takes[op]));
        }

        cnt = PopCnt(control & ~bb_excluded);                 // get mobility count
        mob_mg += Par.r_mob_mg[cnt];
        mob_eg += Par.r_mob_eg[cnt];

        // FILE EVALUATION:

        file = BB.FillNorth(SqBb(sq)) | BB.FillSouth(SqBb(sq));   // get file

        if (file & p->Queens(op)) {                           // enemy queen on rook's file
            lines_mg += V(ROQ_MG);
            lines_eg += V(ROQ_EG);
        }

        if (!(file & p->Pawns(sd))) {                         // no own pawns on that file
            if (!(file & p->Pawns(op))) {                     // open file
                lines_mg += V(ROF_MG);
                lines_eg += V(ROF_EG);
            } else {                                             // half-open file...
                if (file & (p->Pawns(op) & e->p_takes[op])) { // ...with defended enemy pawn
                    lines_mg += V(RBH_MG);
                    lines_eg += V(RBH_EG);
                } else {                                         // ...with undefended enemy pawn
                    lines_mg += V(RGH_MG);
                    lines_eg += V(RGH_EG);
                }
            }
        }

        // Rook on the 7th rank attacking pawns or cutting off enemy king

        if (SqBb(sq) & bb_rel_rank[sd][RANK_7]) {                // rook on 7th rank
            if (p->Pawns(op) & bb_rel_rank[sd][RANK_7]           // attacking enemy pawns
            ||  p->Kings(op) & bb_rel_rank[sd][RANK_8]) {        // or cutting off enemy king
                lines_mg += V(RSR_MG);
                lines_eg += V(RSR_EG);
                r_on_7th++;
            }
        }
    }

    // Queen eval

    pieces = p->Queens(sd);
    while (pieces) {
        sq = PopFirstBit(&pieces);                           // get square

        // queen tropism to enemy king (based on Hakapeliitta)

        tropism += Dist.queenTropism[sq][king_sq];

        if (SqBb(sq) & Mask.away[sd]) {                          // forwardness (based on Toga II 3.0)
            fwd_weight += V(Q_FWD);
            fwd_cnt += 1;
        }

        control = BB.QueenAttacks(p->OccBb(), sq);            // get control bitboard
        e->all_att[sd] |= control;                            // update attack map
        if (control & q_checks) {                             // check threat bonus
            e->att[sd] += V(Q_CHK);

            contact = control & BB.KingAttacks(king_sq);   // queen contact checks
            while (contact) {
                int contactSq = PopFirstBit(&contact);        // find potential contact check square
                if (p->Swap(sq, contactSq) >= 0) {               // if check doesn't lose material, evaluate
                    e->att[sd] += V(Q_CONTACT);
                    break;
                }
            }
        }

        attack  = BB.BishAttacks(p->OccBb() ^ p->DiagMovers(sd), sq);
        attack |= BB.RookAttacks(p->OccBb() ^ p->StraightMovers(sd), sq);

        if (attack & kingZone) {                               // evaluate king attacks
            e->wood[sd]++;
            e->att[sd] += V(Q_ATT1) * PopCnt(attack & (kingZone & ~e->p_takes[op]));
            e->att[sd] += V(Q_ATT2) * PopCnt(attack & (kingZone & e->p_takes[op]));
        }

        cnt = PopCnt(control & ~bb_excluded);                 // get mobility count
        mob_mg += Par.q_mob_mg[cnt];
        mob_eg += Par.q_mob_eg[cnt];

        if (SqBb(sq) & bb_rel_rank[sd][RANK_7]) {                // queen on 7th rank
            if (p->Pawns(op) & bb_rel_rank[sd][RANK_7]           // attacking enemy pawns
            ||  p->Kings(op) & bb_rel_rank[sd][RANK_8]) {        // or cutting off enemy king
                lines_mg += V(QSR_MG);
                lines_eg += V(QSR_EG);
            }
        }

	  // premature queen developement penalty

		U64 b = SqBb(sq);

		if (b & ~(bb_rel_rank[sd][RANK_1] | bb_rel_rank[sd][RANK_2]) ) {
			int tmp = 0;
			if (p->IsOnSq(sd, N, REL_SQ(sd, B1))) tmp -= 2;
			if (p->IsOnSq(sd, N, REL_SQ(sd, G1))) tmp -= 2;
			if (p->IsOnSq(sd, B, REL_SQ(sd, C1))) tmp -= 2;
			if (p->IsOnSq(sd, B, REL_SQ(sd, F1))) tmp -= 2;
			if (p->IsOnSq(sd, P, REL_SQ(sd, E2))) tmp -= 3;
			if (p->IsOnSq(sd, P, REL_SQ(sd, D2))) tmp -= 3;
			Add(e, sd, tmp, 0);
		}

    } // end of queen eval

    // Composite factors

    if (r_on_7th > 1) {  // two rooks on 7th rank
        lines_mg += V(RS2_MG);
        lines_eg += V(RS2_EG);
    }

    // Weighting eval parameters

    Add(e, sd, (Par.sideMobility[sd] * mob_mg)  / 100, (Par.sideMobility[sd] * mob_eg)  / 100);
    Add(e, sd, (V(W_TROPISM) * tropism) / 100, 0);
    Add(e, sd, (V(W_LINES) * lines_mg)     / 100, (V(W_LINES) * lines_eg)     / 100);
    Add(e, sd, (V(W_FWD) * fwd_bonus[fwd_cnt] * fwd_weight) / 100, 0);
    Add(e, sd, (V(W_OUTPOSTS) * outpost_mg) / 100, (V(W_OUTPOSTS) * outpost_eg) / 100);
    Add(e, sd, (V(W_CENTER) * center_control) / 100, 0);

}

void cEngine::EvaluateKingAttack(POS *p, eData *e, eColor sd) {

	// attack on enemy king

    if (e->wood[sd] > 1) {
        if (e->att[sd] > 399) e->att[sd] = 399;
        if (p->mCnt[sd][Q] == 0) e->att[sd] = 0;
        Add(e, sd, (Par.danger[e->att[sd]] * Par.sideAttack[sd]) / 100);
    }
}

void cEngine::EvaluateShielded(POS *p, eData *e, eColor sd, int sq, int v1, int v2, int *outpost_mg, int *outpost_eg) {

    if (SqBb(sq) & Mask.home[sd]) {
        U64 stop = BB.ShiftFwd(SqBb(sq), sd);             // get square in front of a minor
		if (stop & (p->Pawns(sd) | p->Pawns(~sd))) {      // is it occupied by a pawn?
			*outpost_mg += v1;                            // add bonus for a pawn shielding a minor
			*outpost_eg += v2;
		}
    }
}

void cEngine::EvaluateOutpost(POS *p, eData *e, eColor sd, int pc, int sq, int *outpost_mg, int *outpost_eg) {

    int tmp = Par.sp_pst[sd][pc][sq];                     // get base outpost bonus
	int dst = Dist.metric[sd][p->KingSq(~sd)];            // factor in distance to enemy king
	if (dst > 0) tmp += dst / 2;

    if (tmp) {
        int mul = 0;                                      // reset outpost multiplier
        if (SqBb(sq) & ~e->p_can_take[~sd]) mul += 2;     // is piece in hole of enemy pawn structure?
        if (SqBb(sq) & e->p_takes[sd]) mul += 1;          // is piece defended by own pawn?
        if (SqBb(sq) & e->two_pawns_take[sd]) mul += 1;   // is piece defended by two pawns?
        *outpost_mg += ((tmp * mul) / 2);                 // add outpost bonus
		*outpost_eg += (tmp * mul) / 2;
    }
}

void cEngine::EvaluatePawns(POS *p, eData *e, eColor sd) {

    U64 bb_pieces, front_span, fl_phalanx, fl_defended;
    int sq, fl_unopposed;
    eColor op = ~sd;
    int mass_mg = 0;
    int mass_eg = 0;

    bb_pieces = p->Pawns(sd);
    while (bb_pieces) {

        // Set data and flags

        sq = PopFirstBit(&bb_pieces);
        front_span = BB.GetFrontSpan(SqBb(sq), sd);
        fl_unopposed = ((front_span & p->Pawns(op)) == 0);
        fl_phalanx = (ShiftSideways(SqBb(sq)) & p->Pawns(sd));
        fl_defended = (SqBb(sq) & e->p_takes[sd]);

        // Candidate passers

        if (fl_unopposed) {
            if (fl_phalanx || fl_defended) {
                if (PopCnt((Mask.passed[sd][sq] & p->Pawns(op))) == 1)
                    AddPawns(e, sd, Par.cand_bonus_mg[sd][Rank(sq)], Par.cand_bonus_eg[sd][Rank(sq)]);
            }
        }

        // Doubled pawn

        if (front_span & p->Pawns(sd))
            AddPawns(e, sd, V(DB_MID), V(DB_END));

        // Supported pawn

        if (fl_phalanx) {
            mass_mg += Par.sp_pst[sd][PHA_MG][sq];
            mass_eg += Par.sp_pst[sd][PHA_EG][sq];
        } else if (fl_defended) {
            mass_mg += Par.sp_pst[sd][DEF_MG][sq];
            mass_eg += Par.sp_pst[sd][DEF_EG][sq];
        }

        // Isolated and weak pawn

        if (!(Mask.adjacent[File(sq)] & p->Pawns(sd)))
            AddPawns(e, sd, V(ISO_MG) + V(ISO_OF) * fl_unopposed, V(ISO_EG));
        else if (!(Mask.supported[sd][sq] & p->Pawns(sd)))
            AddPawns(e, sd, Par.backward_malus_mg[File(sq)] +V(BK_OPE) * fl_unopposed, V(BK_END));
    }

    AddPawns(e, sd, (mass_mg * V(W_MASS)) / 100, (mass_eg * V(W_MASS)) / 100);
}

void cEngine::EvaluatePassers(POS *p, eData *e, eColor sd) {

    U64 bb_pieces, bb_pawn, bb_stop;
    int sq, mg_tmp, eg_tmp, mul;
    eColor op = ~sd;
    int mg_tot = 0;
    int eg_tot = 0;

    bb_pieces = p->Pawns(sd);
    while (bb_pieces) {
        sq = PopFirstBit(&bb_pieces);
        bb_pawn = SqBb(sq);
        bb_stop = BB.ShiftFwd(SqBb(sq), sd);

        // pawn mobility

        if (!(bb_stop & p->OccBb())) {
			Add(e, sd, V(P_MOB_MG), V(P_MOB_EG));        // pawn mobility bonus
			if (bb_pawn & Mask.center) Add(e, sd, 2, 0); // additional central pawn mobility bonus

			// TODO: pawn can attack enemy piece in one move
        }

        // passed pawns

        if (!(Mask.passed[sd][sq] & p->Pawns(op))) {
            mul = 100;

            if (bb_pawn & e->p_takes[sd]) mul += V(P_DEFMUL);
            if (bb_stop & e->p_takes[sd]) mul += V(P_STOPMUL);

            if (bb_stop & p->OccBb()) mul -= V(P_BL_MUL);   // blocked passers score less

            else if ((bb_stop & e->all_att[sd])  // our control of stop square
                 && (bb_stop & ~e->all_att[op])) mul += V(P_OURSTOP_MUL);

            else if ((bb_stop & e->all_att[op])  // opp control of stop square
                 && (bb_stop & ~e->all_att[sd])) mul -= V(P_OPPSTOP_MUL);

            // in the midgame, we use just a bonus from the table
            // in the endgame, passed pawn attracts both kings.

            mg_tmp = Par.passed_bonus_mg[sd][Rank(sq)];
            eg_tmp = Par.passed_bonus_eg[sd][Rank(sq)]
                     - ((Par.passed_bonus_eg[sd][Rank(sq)] * Dist.bonus[sq][p->mKingSq[op]]) / 30)
                     + ((Par.passed_bonus_eg[sd][Rank(sq)] * Dist.bonus[sq][p->mKingSq[sd]]) / 90);

            mg_tot += (mg_tmp * mul) / 100;
            eg_tot += (eg_tmp * mul) / 100;
        }
    }

    Add(e, sd, (mg_tot * V(W_PASSERS)) / 100, (eg_tot * V(W_PASSERS)) / 100);
}

void cEngine::EvaluateUnstoppable(eData *e, POS *p) {

    U64 bb_pieces, bb_span;
    int w_dist = 8;
    int b_dist = 8;
    int sq, king_sq, pawn_sq, tempo, prom_dist;

    // White unstoppable passers

    if (p->mCnt[BC][N] + p->mCnt[BC][B] + p->mCnt[BC][R] + p->mCnt[BC][Q] == 0) {
        king_sq = p->KingSq(BC);
        if (p->mSide == BC) tempo = 1; else tempo = 0;
        bb_pieces = p->Pawns(WC);
        while (bb_pieces) {
            sq = PopFirstBit(&bb_pieces);
            if (!(Mask.passed[WC][sq] & p->Pawns(BC))) {
                bb_span = BB.GetFrontSpan(SqBb(sq), WC);
                pawn_sq = ((WC - 1) & 56) + (sq & 7);
                prom_dist = Min(5, Dist.metric[sq][pawn_sq]);

                if (prom_dist < (Dist.metric[king_sq][pawn_sq] - tempo)) {
                    if (bb_span & p->Kings(WC)) prom_dist++;
                    w_dist = Min(w_dist, prom_dist);
                }
            }
        }
    }

    // Black unstoppable passers

    if (p->mCnt[WC][N] + p->mCnt[WC][B] + p->mCnt[WC][R] + p->mCnt[WC][Q] == 0) {
        king_sq = p->KingSq(WC);
        if (p->mSide == WC) tempo = 1; else tempo = 0;
        bb_pieces = p->Pawns(BC);
        while (bb_pieces) {
            sq = PopFirstBit(&bb_pieces);
            if (!(Mask.passed[BC][sq] & p->Pawns(WC))) {
                bb_span = BB.GetFrontSpan(SqBb(sq), BC);
                pawn_sq = ((BC - 1) & 56) + (sq & 7);
                prom_dist = Min(5, Dist.metric[sq][pawn_sq]);

                if (prom_dist < (Dist.metric[king_sq][pawn_sq] - tempo)) {
                    if (bb_span & p->Kings(BC)) prom_dist++;
                    b_dist = Min(b_dist, prom_dist);
                }
            }
        }
    }

    if (w_dist < b_dist - 1) Add(e, WC, 0, 500);
    if (b_dist < w_dist - 1) Add(e, BC, 0, 500);
}


void cEngine::Add(eData *e, eColor sd, int mg_val, int eg_val) {

    e->mg[sd] += mg_val;
    e->eg[sd] += eg_val;
}

void cEngine::Add(eData *e, eColor sd, int val) {

    e->mg[sd] += val;
    e->eg[sd] += val;
}

void cEngine::AddPawns(eData *e, eColor sd, int mg_val, int eg_val) {

    e->mg_pawns[sd] += mg_val;
    e->eg_pawns[sd] += eg_val;
}

int cEngine::Interpolate(POS *p, eData *e) {

    int mg_tot = e->mg[WC] - e->mg[BC];
    int eg_tot = e->eg[WC] - e->eg[BC];
    int mg_phase = Min(p->mPhase, 24);
    int eg_phase = 24 - mg_phase;

    return (mg_tot * mg_phase + eg_tot * eg_phase) / 24;
}

void cEngine::EvaluateThreats(POS *p, eData *e, eColor sd) {

    int pc, sq;
    int mg = 0;
    int eg = 0;
    eColor op = ~sd;

    U64 bb_undefended = p->mClBb[op];
    U64 bb_threatened = bb_undefended & e->p_takes[sd];
    U64 bb_defended = bb_undefended & e->all_att[op];
    U64 bb_hanging = bb_undefended & ~e->p_takes[op];

    bb_undefended &= ~e->all_att[sd];
    bb_undefended &= ~e->all_att[op];

    bb_hanging |= bb_threatened;     // piece attacked by our pawn isn't well defended
    bb_hanging &= e->all_att[sd];    // hanging piece has to be attacked

    bb_defended &= e->ev_att[sd];    // N, B, R attacks (pieces attacked by pawns are scored as hanging)
    bb_defended &= ~e->p_takes[sd];  // no defense against pawn attack

	const int att_on_hang_mg[7] = {  0, 15, 15, 17, 25,  0,   0 };
	const int att_on_hang_eg[7] = {  0, 23, 23, 25, 33,  0,   0 };
	const int att_on_def_mg[7]  = {  0,  8,  8, 10, 15,  0,   0 };
	const int att_on_def_eg[7]  = {  0, 12, 12, 14, 19,  0,   0 };
	const int unatt_undef_mg[7] = {  0,  5,  5,  5,  5,  0,   0 };
	const int unatt_undef_eg[7] = {  0,  9,  9,  9,  9,  0,   0 };

    // hanging pieces (attacked and undefended, based on DiscoCheck)

    while (bb_hanging) {
        sq = PopFirstBit(&bb_hanging);
        pc = p->TpOnSq(sq);
        mg += att_on_hang_mg[pc];
        eg += att_on_hang_eg[pc];
    }

    // defended pieces under attack

    while (bb_defended) {
        sq = PopFirstBit(&bb_defended);
        pc = p->TpOnSq(sq);
        mg += att_on_def_mg[pc];
        eg += att_on_def_eg[pc];
    }

    // unattacked and undefended

    while (bb_undefended) {
		sq = PopFirstBit(&bb_undefended);
		pc = p->TpOnSq(sq);
		mg += unatt_undef_mg[pc];
		eg += unatt_undef_eg[pc];
    }

    Add(e, sd, (V(W_THREATS) * mg) / 100, (V(W_THREATS) * eg) / 100);
}

int cEngine::Evaluate(POS *p, eData *e) {

    // Try retrieving score from per-thread eval hashtable

    int addr = p->mHashKey % EVAL_HASH_SIZE;

    if (mEvalTT[addr].key == p->mHashKey) {
        int sc = mEvalTT[addr].score;
        return p->mSide == WC ? sc : -sc;
    }

    // Clear eval data

    e->mg[WC] = p->mMgSc[WC];
    e->mg[BC] = p->mMgSc[BC];
    e->eg[WC] = p->mEgSc[WC];
    e->eg[BC] = p->mEgSc[BC];
	e->att[WC] = e->att[BC] = 0;
	e->wood[WC] = e->wood[BC] = 0;

    // Init helper bitboards (pawn info)

    e->p_takes[WC] = GetWPControl(p->Pawns(WC));
    e->p_takes[BC] = GetBPControl(p->Pawns(BC));
    e->p_can_take[WC] = BB.FillNorth(e->p_takes[WC]);
    e->p_can_take[BC] = BB.FillSouth(e->p_takes[BC]);
    e->two_pawns_take[WC] = GetDoubleWPControl(p->Pawns(WC));
    e->two_pawns_take[BC] = GetDoubleBPControl(p->Pawns(BC));

    // Init or clear attack maps

    e->all_att[WC] = e->p_takes[WC] | BB.KingAttacks(p->KingSq(WC));
    e->all_att[BC] = e->p_takes[BC] | BB.KingAttacks(p->KingSq(BC));
    e->ev_att[WC] = 0ULL;
    e->ev_att[BC] = 0ULL;

    // Run all the evaluation subroutines

    EvaluateMaterial(p, e, WC);
    EvaluateMaterial(p, e, BC);
    EvaluatePieces(p, e, WC);
    EvaluatePieces(p, e, BC);
    EvaluatePawnStruct(p, e);
    EvaluatePassers(p, e, WC);
    EvaluatePassers(p, e, BC);
    EvaluateUnstoppable(e, p);
    EvaluateThreats(p, e, WC);
    EvaluateThreats(p, e, BC);
    Add(e, p->mSide, 14, 7); // tempo bonus

    // Evaluate patterns

    EvaluateKnightPatterns(p, e);
    EvaluateBishopPatterns(p, e);
    EvaluateKingPatterns(p, e);
    EvaluateCentralPatterns(p, e);

	EvaluateKingAttack(p, e, WC);
	EvaluateKingAttack(p, e, BC);

    // Add pawn score (which might come from hash)

    e->mg[WC] += e->mg_pawns[WC];
    e->mg[BC] += e->mg_pawns[BC];
    e->eg[WC] += e->eg_pawns[WC];
    e->eg[BC] += e->eg_pawns[BC];

    // Add asymmetric bonus for keeping certain type of pieces

    e->mg[Par.programSide] += Par.keep_pc[Q] * p->mCnt[Par.programSide][Q];
    e->mg[Par.programSide] += Par.keep_pc[R] * p->mCnt[Par.programSide][R];
    e->mg[Par.programSide] += Par.keep_pc[B] * p->mCnt[Par.programSide][B];
    e->mg[Par.programSide] += Par.keep_pc[N] * p->mCnt[Par.programSide][N];
    e->mg[Par.programSide] += Par.keep_pc[P] * p->mCnt[Par.programSide][P];

    // Interpolate between midgame and endgame scores

    int score = Interpolate(p, e);

    // Material imbalance evaluation (based on Crafty)

    int minor_balance = p->mCnt[WC][N] - p->mCnt[BC][N] + p->mCnt[WC][B] - p->mCnt[BC][B];
    int major_balance = p->mCnt[WC][R] - p->mCnt[BC][R] + 2 * p->mCnt[WC][Q] - 2 * p->mCnt[BC][Q];

    int x = Max(major_balance + 4, 0);
    if (x > 8) x = 8;

    int y = Max(minor_balance + 4, 0);
    if (y > 8) y = 8;

    score += Par.imbalance[x][y];

    // Weakening: add pseudo-random value to eval score

    if (Par.eval_blur) {
        int rand_mod = (Par.eval_blur / 2) - ( (p->mHashKey ^ Glob.game_key) % Par.eval_blur);
        score += rand_mod;
    }

    // Special case code for KBN vs K checkmate

    score += CheckmateHelper(p);

    // Decrease score for drawish endgames

    int draw_factor = 64;
    if (score > 0) draw_factor = GetDrawFactor(p, WC);
    if (score < 0) draw_factor = GetDrawFactor(p, BC);
    score = (score * draw_factor) / 64;


    // Ensure that returned value doesn't exceed mate score

    score = Clip(score, MAX_EVAL);

    // Save eval score in the evaluation hash table

    mEvalTT[addr].key = p->mHashKey;
    mEvalTT[addr].score = score;

    // Return score relative to the side to move

    return p->mSide == WC ? score : -score;
}
