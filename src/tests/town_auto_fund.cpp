/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

/** @file town_auto_fund.cpp Automatic town funding command and monthly payment regressions. */

#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../command_func.h"
#include "../company_base.h"
#include "../core/backup_type.hpp"
#include "../core/random_func.hpp"
#include "../economy_func.h"
#include "../town.h"
#include "../town_cmd.h"

extern void TownsMonthlyLoop();

struct TownAutoFundFixture {
	AutoRestoreBackup<EconomySettings> settings{_settings_game.economy, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup<CompanyID> current_company{_current_company, COMPANY_SPECTATOR};
	AutoRestoreBackup<CompanyID> local_company{_local_company, COMPANY_SPECTATOR};
	AutoRestoreBackup<Money> action_price{_price[Price::TownAction], Money{256}};
	AutoRestoreBackup<Money> station_price{_price[Price::StationValue], Money{2}};
	AutoRestoreBackup<Randomizer> random{_random, AutoRestoreBackupNoNewValueTag{}};
	Town *town = nullptr;
	Company *first = nullptr;
	Company *second = nullptr;

	TownAutoFundFixture()
	{
		REQUIRE(Town::GetNumItems() == 0);
		REQUIRE(Company::GetNumItems() == 0);
		AllocateMap(64, 64);
		town = Town::Create(TileXY(32, 32));
		first = Company::Create();
		second = Company::Create();
		first->money = second->money = 10000;
		_settings_game.economy.fund_buildings = true;
		_settings_game.economy.bribe = true;
	}

	~TownAutoFundFixture()
	{
		_town_pool.CleanPool();
		_company_pool.CleanPool();
	}

	void Enable(Company *company)
	{
		AutoRestoreBackup<CompanyID> owner{_current_company, company->index};
		REQUIRE(Command<Commands::TownAutoFundBuildings>::Do(DoCommandFlag::Execute, town->index, true).Succeeded());
	}

	Money Cost() const { return GetTownActionCost(TownAction::FundBuildings); }
};

TEST_CASE("Automatic town funding toggle validates owners and allows disabling when funding is forbidden", "[town][auto_fund]")
{
	TownAutoFundFixture f;
	CHECK(Command<Commands::TownAutoFundBuildings>::Do(DoCommandFlag::Execute, f.town->index, true).Failed());
	_current_company = f.first->index;
	CHECK(Command<Commands::TownAutoFundBuildings>::Do({}, f.town->index, true).Succeeded());
	CHECK_FALSE(f.town->fund_buildings_auto.Any());
	f.Enable(f.first);
	CHECK(f.town->fund_buildings_auto.Test(f.first->index));
	CHECK_FALSE(f.town->fund_buildings_auto.Test(f.second->index));
	CHECK(f.first->money == 10000);
	CHECK(f.town->fund_buildings_months == 0);
	_settings_game.economy.fund_buildings = false;
	CHECK(Command<Commands::TownAutoFundBuildings>::Do(DoCommandFlag::Execute, f.town->index, true).Failed());
	CHECK(Command<Commands::TownAutoFundBuildings>::Do(DoCommandFlag::Execute, f.town->index, false).Succeeded());
	CHECK_FALSE(f.town->fund_buildings_auto.Any());
	CHECK(Command<Commands::TownAutoFundBuildings>::Do(DoCommandFlag::Execute, TownID::Invalid(), true).Failed());
}

TEST_CASE("Automatic town funding renews once on expiry and charges only one company", "[town][auto_fund]")
{
	TownAutoFundFixture f;
	f.Enable(f.first);
	f.Enable(f.second);
	f.town->fund_buildings_months = 2;
	TownsMonthlyLoop();
	CHECK(f.town->fund_buildings_months == 1);
	CHECK(f.first->money == 10000);
	CHECK(f.second->money == 10000);
	TownsMonthlyLoop();
	CHECK(f.town->fund_buildings_months == 3);
	CHECK(f.first->money == 10000 - f.Cost());
	CHECK(f.second->money == 10000);
	CHECK(_current_company == COMPANY_SPECTATOR);
	TownsMonthlyLoop();
	CHECK(f.town->fund_buildings_months == 2);
	CHECK(f.first->money == 10000 - f.Cost());
}

TEST_CASE("Automatic town funding retries after funding restrictions or insufficient money", "[town][auto_fund]")
{
	TownAutoFundFixture f;
	f.Enable(f.first);
	SECTION("Insufficient money") {
		f.first->money = f.Cost() - 1;
		/* The UI still offers the action; the command must refuse payment. */
		CHECK(GetMaskOfTownActions(f.first->index, f.town).Test(TownAction::FundBuildings));
	}
	SECTION("Failed bribe") { f.town->unwanted[f.first->index] = 2; }
	SECTION("Funding disabled") { _settings_game.economy.fund_buildings = false; }
	const Money before = f.first->money;
	TownsMonthlyLoop();
	CHECK(f.town->fund_buildings_months == 0);
	CHECK(f.first->money == before);
	CHECK(f.town->fund_buildings_auto.Test(f.first->index));
	f.first->money = 10000;
	f.town->unwanted[f.first->index] = 0;
	_settings_game.economy.fund_buildings = true;
	TownsMonthlyLoop();
	CHECK(f.town->fund_buildings_months == 3);
	CHECK(f.first->money == 10000 - f.Cost());
}

TEST_CASE("Automatic town funding skips an ineligible company and a removed company", "[town][auto_fund]")
{
	TownAutoFundFixture f;
	f.Enable(f.first);
	f.Enable(f.second);
	SECTION("First company cannot pay") { f.first->money = 0; }
	SECTION("First company no longer exists") { delete f.first; f.first = nullptr; }
	TownsMonthlyLoop();
	CHECK(f.town->fund_buildings_months == 3);
	CHECK(f.second->money == 10000 - f.Cost());
	CHECK(_current_company == COMPANY_SPECTATOR);
}
