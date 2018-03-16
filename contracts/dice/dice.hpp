/**
 *  @file
 *  @copyright defined in eos/LICENSE.txt
 */

#include <utility>

#include <eosiolib/crypto.h>
#include <eosiolib/types.hpp>
#include <eosiolib/token.hpp>
#include <eosiolib/print.hpp>
#include <eosiolib/db.hpp>
#include <eosiolib/action.hpp>
#include <eosio.system/eosio.system.hpp>
#include <eosiolib/multi_index.hpp>

using eos_currency = eosiosystem::contract<N(eosio)>::currency;

using eosio::key256;
using eosio::indexed_by;
using eosio::const_mem_fun;
using eosio::asset;

template <account_name DiceAccount>
struct dice {

   const uint32_t FIVE_MINUTES = 5*60;

   dice()
   :offers(DiceAccount, DiceAccount),
    games(DiceAccount, DiceAccount),
    global_dices(DiceAccount, DiceAccount),
    accounts(DiceAccount, DiceAccount)
   {}

   //@abi table offer i64
   struct offer {
      uint64_t          id;
      account_name      owner;
      asset             bet;
      checksum256       commitment;
      uint64_t          gameid = 0;
      
      uint64_t primary_key()const { return id; }

      uint64_t by_bet()const { return bet.amount; }
      
      key256 by_commitment()const { return get_commitment(commitment); }

      static key256 get_commitment(const checksum256& commitment) {
         const uint64_t *p64 = reinterpret_cast<const uint64_t *>(&commitment);
         return key256::make_from_word_sequence<uint64_t>(p64[0], p64[1], p64[2], p64[3]);
      }

      EOSLIB_SERIALIZE( offer, (id)(owner)(bet)(commitment)(gameid) )
   };

   typedef eosio::multi_index< N(offer), offer,
      indexed_by< N(bet), const_mem_fun<offer, uint64_t, &offer::by_bet > >,
      indexed_by< N(commitment), const_mem_fun<offer, key256,  &offer::by_commitment> >
   > offer_index;

   struct player {
      checksum256 commitment;
      checksum256 reveal;

      EOSLIB_SERIALIZE( player, (commitment)(reveal) )
   };

   //@abi table game i64
   struct game {
      uint64_t id;
      asset    bet;
      time     deadline;
      player   player1;
      player   player2;

      uint64_t primary_key()const { return id; }

      EOSLIB_SERIALIZE( game, (id)(bet)(deadline)(player1)(player2) )
   };
   
   typedef eosio::multi_index< N(game), game> game_index;

   //@abi table global i64
   struct global_dice {
      uint64_t id = 0;
      uint64_t nextgameid = 0;

      uint64_t primary_key()const { return id; }

      EOSLIB_SERIALIZE( global_dice, (id)(nextgameid) )
   };

   typedef eosio::multi_index< N(global), global_dice> global_dice_index;

   //@abi table account i64
   struct account {
      account( account_name o = account_name() ):owner(o){}

      account_name owner;
      asset        eos_balance;
      uint32_t     open_offers = 0;
      uint32_t     open_games = 0;

      bool is_empty()const { return !( eos_balance.amount | open_offers | open_games ); }

      uint64_t primary_key()const { return owner; }

      EOSLIB_SERIALIZE( account, (owner)(eos_balance)(open_offers)(open_games) )
   };
   
   typedef eosio::multi_index< N(account), account> account_index;

   offer_index       offers;
   game_index        games;
   global_dice_index global_dices;
   account_index     accounts;

   bool has_offer( const checksum256& commitment )const {
      auto idx = offers.template get_index<N(commitment)>();
      auto itr = idx.find( offer::get_commitment(commitment) );
      return itr != idx.end();
   }

   bool is_equal(const checksum256& a, const checksum256& b)const {
      return memcmp((void *)&a, (const void *)&b, sizeof(checksum256)) == 0;
   }

   bool is_zero(const checksum256& a)const {
      const uint64_t *p64 = reinterpret_cast<const uint64_t*>(&a);
      return p64[0] == 0 && p64[1] == 0 && p64[2] == 0 && p64[3] == 0;
   }
   
   void pay_and_clean(const game& g, const offer& winner_offer,
       const offer& loser_offer) {

      // Update winner account balance and game count
      auto winner_account = accounts.find(winner_offer.owner);
      accounts.modify( winner_account, 0, [&]( auto& acnt ) {
         acnt.eos_balance.amount += 2*g.bet.amount;
         acnt.open_games--;
      });

      // Update losser account game count
      auto loser_account = accounts.find(loser_offer.owner);
      accounts.modify( loser_account, 0, [&]( auto& acnt ) {
         acnt.open_games--;
      });

      if( loser_account->is_empty() ) {
         accounts.erase(loser_account);
      }

      games.erase(g);
      offers.erase(winner_offer);
      offers.erase(loser_offer);
   }

   //@abi action
   ACTION( DiceAccount, offerbet ) {
      asset         bet;
      account_name  player;
      checksum256   commitment;

      EOSLIB_SERIALIZE( offerbet, (bet)(player)(commitment) )
   };

   void on(const offerbet& new_offer) {

      auto amount = eos_currency::token_type(new_offer.bet);

      eosio_assert( amount.quantity > 0, "invalid bet" );
      eosio_assert( !has_offer( new_offer.commitment ), "offer with this commitment already exist" );
      require_auth( new_offer.player );

      auto cur_player_itr = accounts.find( new_offer.player );
      eosio_assert(cur_player_itr != accounts.end(), "unknown account");
      
      // Store new offer
      auto new_offer_itr = offers.emplace(DiceAccount, [&](auto& offer){
         offer.id         = offers.available_primary_key();
         offer.bet        = new_offer.bet;
         offer.owner      = new_offer.player;
         offer.commitment = new_offer.commitment;
         offer.gameid     = 0;
      });

      // Try to find a matching bet
      auto idx = offers.template get_index<N(bet)>();
      auto matched_offer_itr = idx.lower_bound( new_offer_itr->bet.amount );

      if( matched_offer_itr == idx.end()
         || matched_offer_itr->bet.amount != new_offer_itr->bet.amount
         || matched_offer_itr->owner == new_offer_itr->owner ) {

         // No matching bet found, update player's account
         accounts.modify( cur_player_itr, 0, [&](auto& acnt) {
            acnt.eos_balance = eos_currency::token_type(acnt.eos_balance) - amount;
            acnt.open_offers++;
         });

      } else {
         // Create global game counter if not exists
         auto gdice_itr = global_dices.begin();
         if( gdice_itr == global_dices.end() ) {
            gdice_itr = global_dices.emplace(DiceAccount, [&](auto& gdice){
               gdice.nextgameid=0;
            });
         }

         // Increment global game counter
         global_dices.modify(gdice_itr, 0, [&](auto& gdice){
            gdice.nextgameid++;
         });

         // Create a new game
         auto game_itr = games.emplace(DiceAccount, [&](auto& new_game){
            new_game.id       = gdice_itr->nextgameid;
            new_game.bet      = new_offer_itr->bet;
            new_game.deadline = 0;
            
            new_game.player1.commitment = matched_offer_itr->commitment;
            memset(&new_game.player1.reveal, 0, sizeof(checksum256));
            
            new_game.player2.commitment = new_offer_itr->commitment;
            memset(&new_game.player2.reveal, 0, sizeof(checksum256));
         });

         // Update player's offers
         idx.modify(matched_offer_itr, 0, [&](auto& offer){
            offer.bet.amount = 0;
            offer.gameid = game_itr->id;
         });

         offers.modify(new_offer_itr, 0, [&](auto& offer){
            offer.bet.amount = 0;
            offer.gameid = game_itr->id;
         });

         // Update player's accounts
         accounts.modify( accounts.find( matched_offer_itr->owner ), 0, [&](auto& acnt) {
            acnt.open_offers--;
            acnt.open_games++;
         });

         accounts.modify( cur_player_itr, 0, [&](auto& acnt) {
            acnt.eos_balance = eos_currency::token_type(acnt.eos_balance) - amount;
            acnt.open_games++;
         });
      }
   }

   //@abi action
   ACTION( DiceAccount, canceloffer ) {
      checksum256  commitment;
      
      EOSLIB_SERIALIZE( canceloffer, (commitment) )
   };

   void on( const canceloffer& c) {

      auto idx = offers.template get_index<N(commitment)>();
      auto offer_itr = idx.find( offer::get_commitment(c.commitment) );

      eosio_assert( offer_itr != idx.end(), "offer does not exists" );
      eosio_assert( offer_itr->gameid == 0, "unable to cancel offer" );
      require_auth( offer_itr->owner );

      auto acnt_itr = accounts.find(offer_itr->owner);
      accounts.modify(acnt_itr, 0, [&](auto& acnt){
         acnt.open_offers--;
         acnt.eos_balance.amount += offer_itr->bet.amount;
      });

      idx.erase(offer_itr);
   }

   //@abi action
   ACTION( DiceAccount, reveal ) {
      checksum256  commitment;
      checksum256  source;

      EOSLIB_SERIALIZE( reveal, (commitment)(source) )
   };

   void on( const reveal& reveal_info ) {

      assert_sha256( (char *)&reveal_info.source, sizeof(reveal_info.source), (const checksum256 *)&reveal_info.commitment );

      auto idx = offers.template get_index<N(commitment)>();
      auto curr_revealer_offer = idx.find( offer::get_commitment(reveal_info.commitment)  );

      eosio_assert(curr_revealer_offer != idx.end(), "offer not found");
      eosio_assert(curr_revealer_offer->gameid > 0, "unable to reveal");

      auto game_itr = games.find( curr_revealer_offer->gameid );

      player curr_reveal = game_itr->player1;
      player prev_reveal = game_itr->player2;
      
      if( !is_equal(curr_reveal.commitment, reveal_info.commitment) ) {
         std::swap(curr_reveal, prev_reveal);
      }

      eosio_assert( is_zero(curr_reveal.reveal) == true, "player already revealed");
      
      if( !is_zero(prev_reveal.reveal) ) {
         
         checksum256 result;
         sha256( (char *)&game_itr->player1, sizeof(player)*2, &result);

         auto prev_revealer_offer = idx.find( offer::get_commitment(prev_reveal.commitment) );

         int winner = result.hash[1] < result.hash[0] ? 0 : 1;
         
         if( winner ) {
            pay_and_clean(*game_itr, *curr_revealer_offer, *prev_revealer_offer);
         } else {
            pay_and_clean(*game_itr, *prev_revealer_offer, *curr_revealer_offer);
         }

      } else {
         games.modify(game_itr, 0, [&](auto& game){
            
            if( is_equal(curr_reveal.commitment, game.player1.commitment) )
               game.player1.reveal = reveal_info.source;
            else
               game.player2.reveal = reveal_info.source;

            game.deadline = now() + FIVE_MINUTES;
         });
      }
   }

   //@abi action
   ACTION( DiceAccount, claimexpired ) {
      uint64_t gameid = 0;

      EOSLIB_SERIALIZE( claimexpired, (gameid) )
   };

   void on( const claimexpired& claim ) {

      auto game_itr = games.find(claim.gameid);
      
      eosio_assert(game_itr != games.end(), "game not found");
      eosio_assert(game_itr->deadline != 0 && now() > game_itr->deadline, "game not expired");

      auto idx = offers.template get_index<N(commitment)>();
      auto player1_offer = idx.find( offer::get_commitment(game_itr->player1.commitment) );
      auto player2_offer = idx.find( offer::get_commitment(game_itr->player2.commitment) );

      if( !is_zero(game_itr->player1.reveal) ) {
         eosio_assert( is_zero(game_itr->player2.reveal), "game error");
         pay_and_clean(*game_itr, *player1_offer, *player2_offer);
      } else {
         eosio_assert( is_zero(game_itr->player1.reveal), "game error");
         pay_and_clean(*game_itr, *player2_offer, *player1_offer);
      }
      
   }

   //@abi action
   ACTION( DiceAccount, deposit ) {
      account_name from;
      asset        amount;

      EOSLIB_SERIALIZE( deposit, (from)(amount) )
   };

   void on( const deposit& d ) {
   
      auto itr = accounts.find(d.from);
      if( itr == accounts.end() ) {
         itr = accounts.emplace(DiceAccount, [&](auto& acnt){
            acnt.owner = d.from;
         });
      }

      auto amount = eos_currency::token_type(d.amount);

      eos_currency::inline_transfer( d.from, DiceAccount, amount );
      accounts.modify( itr, 0, [&]( auto& acnt ) {
         acnt.eos_balance = eos_currency::token_type(acnt.eos_balance) + amount;
      });
   }

   //@abi action
   ACTION( DiceAccount, withdraw ) {
      account_name to;
      asset        amount;

      EOSLIB_SERIALIZE( withdraw, (to)(amount) )
   };

   void on( const withdraw& w ) {
      require_auth( w.to );

      auto itr = accounts.find( w.to );
      eosio_assert(itr != accounts.end(), "unknown account");

      auto amount = eos_currency::token_type(w.amount);
      accounts.modify( itr, 0, [&]( auto& acnt ) {
         acnt.eos_balance = eos_currency::token_type(acnt.eos_balance) - amount;
      });

      eos_currency::inline_transfer( DiceAccount, w.to, amount );

      if( itr->is_empty() ) {
         accounts.erase(itr);
      }
   }

   static void apply( uint64_t code, uint64_t act ) {
      if(code == current_receiver()) {
         if( !eosio::dispatch<dice,
                offerbet, canceloffer,
                reveal, claimexpired,
                deposit, withdraw
             >( code, act ) ) {
            eosio::print("Unexpected action: ", eosio::name(act), "\n");
            eosio_assert( false, "received unexpected action");
         }
      }
   }
};
