project (TeamTalkClients)

option (BUILD_TEAMTALK_CLIENT_PRISM "Build TeamTalk clients with Prism screen reader support" ON)
if (BUILD_TEAMTALK_CLIENT_PRISM)
  add_subdirectory (Prism)
endif()

if (MSVC)

  add_subdirectory (TeamTalkClassic)
  option (BUILD_TEAMTALK_CLIENT_NET "Build .NET client examples" OFF)
  if (CMAKE_CSharp_COMPILER AND BUILD_TEAMTALK_CLIENT_NET)
    add_subdirectory (TeamTalkApp.NET)
    add_subdirectory (ttserverlog.NET)
  else()
    message(STATUS "Skipping .NET client examples")
  endif()
endif()

option (BUILD_TEAMTALK_CLIENT_JAVA "Build Java client examples" OFF)
if (BUILD_TEAMTALK_CLIENT_JAVA)
  add_subdirectory (jSpamBot)
  add_subdirectory (jTeamTalk)
endif()

add_subdirectory (qtTeamTalk)
add_subdirectory (ttserverlog)

